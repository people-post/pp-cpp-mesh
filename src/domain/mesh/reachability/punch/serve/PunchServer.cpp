#include "domain/mesh/reachability/punch/serve/PunchServer.h"

#include "common/metrics/MetricsRegistry.h"

#include "amp/L3/ChannelSession.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/PeerLink.h"
#include "domain/mesh/reachability/punch/PunchLogic.h"
#include "common/Logger.h"
#include "common/ValueJson.h"
#include "foundation/runtime/DeferredSelf.h"
#include "domain/mesh/shared/AmpParkUntil.h"

#include <atomic>
#include <chrono>
#include <optional>

namespace pbr {
namespace {

logging::Logger& AmpPunchLog() {
  static logging::Logger log = logging::getLogger("AmpPunch");
  return log;
}

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return {json_utf8.begin(), json_utf8.end()};
}

std::string MakeEpochId(const std::string& local_peer_id) {
  const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
                         Clock::now().time_since_epoch())
                         .count();
  return "ep-" + local_peer_id.substr(0, std::min<size_t>(local_peer_id.size(), 8)) + "-" +
         std::to_string(ticks);
}

std::string ResolvePeerKey(pp::amp::PeerLinkManager& links, const std::string& peer_id) {
  if (peer_id.empty()) {
    return {};
  }
  if (auto* link = links.FindLinkByPeerId(peer_id)) {
    return link->PeerKey();
  }
  if (links.GetLinkSnapshot(peer_id).has_endpoint) {
    return peer_id;
  }
  if (auto ma = links.PreferredMultiaddr(peer_id)) {
    (void)links.RegisterEndpoint(peer_id, *ma);
    return peer_id;
  }
  return {};
}

} // namespace

struct PunchServer::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  IoPump io_pump; // AmpParkUntil only — never call from SM / PostToIo work
  std::atomic<bool> stopped{false};
  /** Guards protocol-handler raw Impl* past Stop — OWNERSHIP.md § DeferredSelf. */
  DeferredSelf deferred;
  std::vector<std::string>* local_addrs = nullptr;
  std::atomic<const AddressDisclosureGate*> disclosure{nullptr};

  pp::amp::PeerLinkManager& Links() { return runtime->Links(); }

  std::function<void(std::function<void()>)> MakePostIo() {
    return [this](std::function<void()> fn) { PostStrand(std::move(fn)); };
  }

  /** Non-teardown SM work on MeshRuntime IO strand (connect/offer continuations). */
  void PostStrand(std::function<void()> fn) {
    if (!fn) {
      return;
    }
    if (runtime) {
      runtime->PostToIo(std::move(fn));
      return;
    }
    fn();
  }

  /** Teardown lane — Abort / Close / on_done (MeshRuntime::PostDeferred). */
  void PostDeferred(std::function<void()> fn) {
    if (!fn) {
      return;
    }
    if (runtime) {
      runtime->PostDeferred(std::move(fn));
      return;
    }
    fn();
  }

  void FailSession(const std::shared_ptr<pp::amp::ChannelSession>& session, const std::string& epoch_id,
                   const std::string& error) {
    AmpPunchLog().warning << "punch introduce failed epoch=" << epoch_id << " error=" << error;
    PunchResult result;
    result.epoch_id = epoch_id;
    result.ok = false;
    result.error = error;
    (void)session->EnqueueOutbound(JsonToBody(EncodePunchResult(result)));
    PostDeferred([session]() {
      if (session) {
        session->Close();
      }
    });
  }

  /** The endpoint our ADP link to `peer_id` sees — the peer's NAT mapping toward us. */
  std::optional<std::string> ObservedAddrFor(const std::string& peer_id) {
    if (peer_id.empty()) {
      return std::nullopt;
    }
    auto* link = Links().FindConnectedLinkByPeerId(peer_id, pp::amp::TransportClass::Adp);
    const auto* conn = link ? link->ConnectionOrNull() : nullptr;
    if (!conn) {
      return std::nullopt;
    }
    auto ma = pp::amp::FormatAdpMultiaddr(conn->PeerEndpoint(), peer_id);
    return ma ? std::optional<std::string>(*ma) : std::nullopt;
  }

  void SendSyncPair(const std::shared_ptr<pp::amp::ChannelSession>& initiator_session,
                    const std::shared_ptr<pp::amp::ChannelSession>& target_session,
                    const std::string& initiator_peer_id, const std::string& epoch_id, int window_ms,
                    const PunchConnectRequest& req, const PunchCandidates& candidates) {
    // Each side bursts to the other's self-reported candidates led by what we observe for it.
    PunchSync sync_to_initiator;
    sync_to_initiator.epoch_id = epoch_id;
    sync_to_initiator.peer_addrs = WithObservedPunchAddr(ObservedAddrFor(req.target_peer_id), candidates.addrs);
    sync_to_initiator.window_ms = window_ms;

    PunchSync sync_to_target;
    sync_to_target.epoch_id = epoch_id;
    sync_to_target.peer_addrs = WithObservedPunchAddr(ObservedAddrFor(initiator_peer_id), req.addrs);
    AmpPunchLog().info << "punch introduce epoch=" << epoch_id << " initiator_addrs="
                       << (sync_to_target.peer_addrs.empty() ? std::string("-") : sync_to_target.peer_addrs.front())
                       << " (" << sync_to_target.peer_addrs.size() << ") target_addrs="
                       << (sync_to_initiator.peer_addrs.empty() ? std::string("-") : sync_to_initiator.peer_addrs.front())
                       << " (" << sync_to_initiator.peer_addrs.size() << ")";
    sync_to_target.window_ms = window_ms;

    if (!initiator_session->EnqueueOutbound(JsonToBody(EncodePunchSync(sync_to_initiator)))) {
      FailSession(initiator_session, epoch_id, "punch: failed to send sync to initiator");
      return;
    }
    if (target_session && !target_session->IsClosed()) {
      if (!target_session->EnqueueOutbound(JsonToBody(EncodePunchSync(sync_to_target)))) {
        FailSession(initiator_session, epoch_id, "punch: failed to send sync to target");
        return;
      }
    }
    // Do not call IoPump/Tick here — next exclusive Drive flushes outbound.
  }

  /**
   * Introducer side of ACP: open punch channel to target, offer, then sync both ends.
   * Fully async — no AmpParkUntil. Continuations run on the IO strand via callbacks / PostStrand.
   */
  void RunIntroducerConnect(const std::shared_ptr<pp::amp::ChannelSession>& initiator_session,
                            const std::string& initiator_peer_id, PunchConnectRequest req) {
    if (stopped.load(std::memory_order_acquire) || !runtime) {
      return;
    }
    const int window_ms = req.window_ms > 0 ? req.window_ms : 2000;
    const std::string epoch_id = MakeEpochId(Links().LocalPeerId());
    const auto deadline = Clock::now() + std::chrono::milliseconds(window_ms + 3000);

    const std::string target_key = ResolvePeerKey(Links(), req.target_peer_id);
    if (target_key.empty()) {
      FailSession(initiator_session, epoch_id, "punch: target endpoint unknown to introducer");
      return;
    }

    auto target_session = std::make_shared<pp::amp::ChannelSession>();
    auto target_settled = std::make_shared<std::atomic<bool>>(false);
    auto finish_candidates =
        [this, target_settled, target_session, initiator_session, initiator_peer_id, epoch_id, window_ms,
         req](CodedRoe<PunchCandidates, PunchErr> value) {
          if (target_settled->exchange(true, std::memory_order_acq_rel)) {
            return;
          }
          // Stay on strand: candidate frames may arrive under mux — continue via PostStrand.
          PostStrand([this, target_session, initiator_session, initiator_peer_id, epoch_id, window_ms, req,
                      value = std::move(value)]() mutable {
            if (stopped.load(std::memory_order_acquire) || !runtime) {
              target_session->Close();
              return;
            }
            if (!value) {
              target_session->Close();
              FailSession(initiator_session, epoch_id, value.error().message);
              return;
            }
            if (SanitizePunchAddrs(value->addrs).empty()) {
              target_session->Close();
              FailSession(initiator_session, epoch_id, "punch: target returned no candidates");
              return;
            }
            SendSyncPair(initiator_session, target_session, initiator_peer_id, epoch_id, window_ms, req, *value);
          });
        };

    const auto read_timeout = PunchRemainingTimeout(deadline);
    Links().EnsureAssociation(
        target_key, [this, target_key, initiator_peer_id, req, epoch_id, window_ms, finish_candidates,
                     target_session, deadline, read_timeout](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
          if (!assoc) {
            finish_candidates(CodedRoe<PunchCandidates, PunchErr>::error(WrapPunchLinkFailure(assoc.error())));
            return;
          }
          Links().OpenChannel(
              target_key, kAmpPunchProtocolId, PunchJsonChannelPolicy(read_timeout),
              [this, target_key, initiator_peer_id, req, epoch_id, window_ms, finish_candidates, target_session,
               deadline, read_timeout](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
                if (!channel) {
                  finish_candidates(CodedRoe<PunchCandidates, PunchErr>::error(WrapPunchLinkFailure(channel.error())));
                  return;
                }
                AmpScheduleWhenChannelOpen(
                    MakePostIo(), io_pump,
                    [this, target_key, channel_id = *channel]() {
                      auto* link = Links().FindLink(target_key);
                      return link && link->Mux() &&
                             link->Mux()->State(channel_id) == pp::amp::ChannelState::Open;
                    },
                    deadline,
                    [this, target_key, channel_id = *channel, initiator_peer_id, req, epoch_id, window_ms,
                     finish_candidates, target_session, read_timeout](bool open) mutable {
                      if (!open) {
                        finish_candidates(CodedRoe<PunchCandidates, PunchErr>::error(
                            PunchFailure::Of(PunchErr::ChannelFailed, "punch: target channel open failed")));
                        return;
                      }
                      auto* link = Links().FindLink(target_key);
                      if (!link || !link->Mux() ||
                          link->Mux()->State(channel_id) != pp::amp::ChannelState::Open) {
                        finish_candidates(CodedRoe<PunchCandidates, PunchErr>::error(
                            PunchFailure::Of(PunchErr::ChannelFailed, "punch: target channel open failed")));
                        return;
                      }
                      target_session->Bind(
                          *link->Mux(), channel_id, PunchJsonChannelPolicy(read_timeout),
                          [finish_candidates](Roe<std::vector<uint8_t>> frame) {
                            if (!frame) {
                              finish_candidates(CodedRoe<PunchCandidates, PunchErr>::error(
                                  PunchFailure::Of(PunchErr::ProtocolError, "punch: failed to read target candidates")));
                              return false;
                            }
                            auto root = TryParseObject(std::string(frame->begin(), frame->end()));
                            if (!root) {
                              finish_candidates(CodedRoe<PunchCandidates, PunchErr>::error(
                                  PunchFailure::Of(PunchErr::ProtocolError, "punch: invalid target candidates json")));
                              return false;
                            }
                            auto decoded = DecodePunchCandidates(*root);
                            if (!decoded) {
                              finish_candidates(CodedRoe<PunchCandidates, PunchErr>::error(
                                  PunchFailure::Of(PunchErr::ProtocolError, "punch: target candidates decode failed")));
                              return false;
                            }
                            finish_candidates(*decoded);
                            return true; // keep open for sync
                          });

                      PunchOffer offer;
                      offer.initiator_peer_id = initiator_peer_id;
                      offer.addrs = SanitizePunchAddrs(req.addrs);
                      offer.epoch_id = epoch_id;
                      offer.window_ms = window_ms;
                      if (!target_session->EnqueueOutbound(JsonToBody(EncodePunchOffer(offer)))) {
                        finish_candidates(CodedRoe<PunchCandidates, PunchErr>::error(
                            PunchFailure::Of(PunchErr::ProtocolError, "punch: failed to send offer")));
                        return;
                      }
                    },
                    [this]() { return stopped.load(std::memory_order_acquire); });
              });
        });
  }

  void RunBurstAndReply(const std::shared_ptr<pp::amp::ChannelSession>& session, PunchSync sync,
                        std::string remote_peer_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime) {
      return;
    }
    for (const std::string& ma : SanitizePunchAddrs(sync.peer_addrs)) {
      if (auto parsed = pp::amp::ParseAdpMultiaddr(ma)) {
        if (!parsed->peer_id.empty()) {
          (void)Links().RegisterEndpoint(parsed->peer_id, ma);
        }
      }
    }
    auto complete = std::make_shared<std::function<void(PunchBurstResult)>>();
    *complete = [this, session, sync, remote_peer_id](PunchBurstResult burst) mutable {
      if (stopped.load(std::memory_order_acquire) || !runtime || !session) {
        return;
      }
      std::string remote_id = remote_peer_id;
      if (remote_id.empty()) {
        for (const std::string& ma : sync.peer_addrs) {
          if (auto parsed = pp::amp::ParseAdpMultiaddr(ma)) {
            remote_id = parsed->peer_id;
            break;
          }
        }
      }
      PublishIfPunchConnected(Links(), remote_id, burst);
      PunchResult result;
      result.epoch_id = sync.epoch_id;
      result.ok = burst.ok;
      result.winner_multiaddr = burst.dialed;
      result.error = burst.ok ? "" : burst.error;
      (void)session->EnqueueOutbound(JsonToBody(EncodePunchResult(result)));
      PostDeferred([session]() {
        if (session) {
          session->Close();
        }
      });
    };
    const int window = sync.window_ms > 0 ? sync.window_ms : 2000;
    runtime->BurstDial(sync.peer_addrs, std::chrono::milliseconds(window),
                       [complete](pp::amp::BurstDialResult r) { (*complete)(ToPunchBurst(std::move(r))); });
  }

  /** pp_punch_served_total{role} (docs/contracts/NODE_METRICS.md § Reachability). */
  static void CountServed(const char* role) {
    MetricsRegistry::Global()
        .Counter("pp_punch_served_total", "Punch requests this node served, by role.", {{"role", role}})
        .Inc();
  }

  void HandleInboundFrame(const std::shared_ptr<pp::amp::ChannelSession>& session,
                          const std::string& remote_peer_id, const std::shared_ptr<std::string>& phase,
                          const std::shared_ptr<std::string>& punch_remote_peer_id,
                          const std::string& json_utf8) {
    if (stopped.load(std::memory_order_acquire) || !runtime || !session) {
      return;
    }
    auto root = TryParseObject(json_utf8);
    if (!root) {
      return;
    }
    const std::string op = PunchOp(*root).value_or("");
    if (op == "connect" && *phase == "await_first") {
      auto req = DecodePunchConnect(*root);
      if (!req) {
        FailSession(session, "", "punch: invalid connect");
        return;
      }
      *phase = "introducing";
      CountServed("introducer");
      RunIntroducerConnect(session, remote_peer_id, *req);
      return;
    }
    if (op == "offer" && *phase == "await_first") {
      auto offer = DecodePunchOffer(*root);
      if (!offer) {
        FailSession(session, "", "punch: invalid offer");
        return;
      }
      if (!AllowsDirect(disclosure.load(std::memory_order_acquire), offer->initiator_peer_id)) {
        // projects/privacy T1: our candidates and burst would hand this initiator our IP.
        CountServed("target_declined");
        FailSession(session, offer->epoch_id, "punch: target declines this initiator");
        return;
      }
      *phase = "await_sync";
      CountServed("target");
      *punch_remote_peer_id = offer->initiator_peer_id;
      PunchCandidates reply;
      reply.peer_id = Links().LocalPeerId();
      reply.addrs = local_addrs ? SanitizePunchAddrs(*local_addrs) : std::vector<std::string>{};
      reply.nonce = offer->epoch_id;
      if (!session->EnqueueOutbound(JsonToBody(EncodePunchCandidates(reply)))) {
        FailSession(session, offer->epoch_id, "punch: failed to send candidates");
        return;
      }
      return;
    }
    if (op == "sync" && *phase == "await_sync") {
      auto sync = DecodePunchSync(*root);
      if (!sync) {
        FailSession(session, "", "punch: invalid sync");
        return;
      }
      *phase = "bursting";
      RunBurstAndReply(session, *sync, *punch_remote_peer_id);
      return;
    }
  }

  void HandleInboundOnLink(pp::amp::LinkHandle /*handle*/, const std::string& remote_peer_id_in,
                           uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime || remote_peer_id_in.empty()) {
      return;
    }
    const std::string remote_peer_id = remote_peer_id_in;
    auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    auto phase = std::make_shared<std::string>("await_first");
    auto punch_remote_peer_id = std::make_shared<std::string>();
    *session_holder = Links().BindChannel(
        remote_peer_id, channel_id, PunchJsonChannelPolicy(std::chrono::milliseconds{8000}),
        [this, session_holder, remote_peer_id, phase, punch_remote_peer_id](Roe<std::vector<uint8_t>> frame) {
          auto session = *session_holder;
          if (!session || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          const std::string json_utf8(frame->begin(), frame->end());
          // Leave the mux stack before any dial/park/burst (Windows nested Drive UAF).
          PostStrand([this, session, remote_peer_id, phase, punch_remote_peer_id, json_utf8]() {
            HandleInboundFrame(session, remote_peer_id, phase, punch_remote_peer_id, json_utf8);
          });
          return true; // keep open across connect/offer → sync
        });
  }
};

PunchServer::PunchServer(pp::amp::MeshRuntime& runtime, IoPump io_pump)
    : impl_(std::make_unique<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
  impl_->io_pump = std::move(io_pump);
  impl_->local_addrs = &local_addrs_;
}

PunchServer::~PunchServer() { Stop(); }

void PunchServer::SetAddressDisclosure(const AddressDisclosureGate* gate) {
  impl_->disclosure.store(gate, std::memory_order_release);
}

void PunchServer::SetLocalCandidateAddrs(std::vector<std::string> addrs) {
  local_addrs_ = SanitizePunchAddrs(std::move(addrs));
}

void PunchServer::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  runtime_.Links().SetProtocolHandler(
      kAmpPunchProtocolId,
      impl_->deferred.Bind([impl = impl_.get()](pp::amp::LinkHandle handle, const std::string& remote_peer_id,
                                                uint32_t channel_id) {
        impl->HandleInboundOnLink(handle, remote_peer_id, channel_id);
      }));
}

void PunchServer::Stop() {
  // Idempotent: the destructor Stops again, possibly after MeshHost::Stop freed the runtime.
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.Links().RemoveProtocolHandler(kAmpPunchProtocolId);
  impl_->deferred.Invalidate();
}

} // namespace pbr
