#include "domain/mesh/reachability/punch/client/PunchClientCoordinator.h"

#include "amp/L3/ChannelSession.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/PeerLink.h"
#include "domain/mesh/reachability/punch/PunchLogic.h"
#include "common/SettledWait.h"
#include "common/ValueJson.h"
#include "domain/mesh/shared/AmpParkUntil.h"

#include <atomic>
#include <chrono>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return {json_utf8.begin(), json_utf8.end()};
}

} // namespace

PunchClientCoordinator::PunchClientCoordinator(pp::amp::MeshRuntime& runtime, IoPump io_pump)
    : runtime_(runtime), io_pump_(std::move(io_pump)) {}

void PunchClientCoordinator::TryColdPunchAsync(const std::string& introducer_peer_key,
                                            const std::string& target_peer_id,
                                            const std::vector<std::string>& my_addrs,
                                            std::function<void(PunchRoe)> on_done, int window_ms) {
  RunPunchAsync(introducer_peer_key, target_peer_id, my_addrs, window_ms, "cold", std::move(on_done));
}

void PunchClientCoordinator::TryUpgradePunchAsync(const std::string& introducer_peer_key,
                                               const std::string& target_peer_id,
                                               const std::vector<std::string>& my_addrs,
                                               std::function<void(PunchRoe)> on_done, int window_ms) {
  RunPunchAsync(introducer_peer_key, target_peer_id, my_addrs, window_ms, "upgrade", std::move(on_done));
}

void PunchClientCoordinator::TrySignalingPunchBurstAsync(const std::vector<std::string>& peer_addrs,
                                                      std::function<void(PunchRoe)> on_done, int window_ms) {
  if (!on_done) {
    return;
  }
  if (!IsStarted()) {
    on_done(PunchRoe::error(Failure::Of(Err::NotStarted, "punch: not started")));
    return;
  }
  const auto sanitized = DialablePunchAddrs(peer_addrs);
  if (sanitized.empty()) {
    on_done(PunchRoe::error(Failure::Of(Err::InvalidRequest, "punch: no peer candidates")));
    return;
  }
  std::string remote_peer_id;
  for (const std::string& ma : sanitized) {
    if (auto parsed = pp::amp::ParseAdpMultiaddr(ma)) {
      if (!parsed->peer_id.empty()) {
        (void)runtime_.Links().RegisterEndpoint(parsed->peer_id, ma);
        if (remote_peer_id.empty()) {
          remote_peer_id = parsed->peer_id;
        }
      }
    }
  }
  const int burst_window = window_ms > 0 ? window_ms : 2000;
  runtime_.BurstDial(
      sanitized, std::chrono::milliseconds(burst_window),
      [this, on_done = std::move(on_done), remote_peer_id](pp::amp::BurstDialResult r) mutable {
        PunchBurstResult burst = ToPunchBurst(std::move(r));
        PublishIfPunchConnected(runtime_.Links(), remote_peer_id, burst);
        PunchResult result;
        result.epoch_id = "signaling";
        result.ok = burst.ok;
        result.winner_multiaddr = burst.dialed;
        result.error = burst.ok ? "" : burst.error;
        if (burst.ok) {
          on_done(result);
        } else {
          on_done(PunchRoe::error(
              Failure::Of(Err::PunchFailed, burst.error.empty() ? "punch burst failed" : burst.error)));
        }
      });
}

PunchClientCoordinator::PunchRoe PunchClientCoordinator::TryColdPunch(const std::string& introducer_peer_key,
                                                                const std::string& target_peer_id,
                                                                const std::vector<std::string>& my_addrs,
                                                                int window_ms) {
  return RunPunch(introducer_peer_key, target_peer_id, my_addrs, window_ms, "cold");
}

PunchClientCoordinator::PunchRoe PunchClientCoordinator::TryUpgradePunch(const std::string& introducer_peer_key,
                                                                   const std::string& target_peer_id,
                                                                   const std::vector<std::string>& my_addrs,
                                                                   int window_ms) {
  return RunPunch(introducer_peer_key, target_peer_id, my_addrs, window_ms, "upgrade");
}

PunchClientCoordinator::PunchRoe PunchClientCoordinator::RunPunch(const std::string& introducer_peer_key,
                                                            const std::string& target_peer_id,
                                                            const std::vector<std::string>& my_addrs,
                                                            int window_ms, const std::string& reason) {
  SettledWait<PunchResult, Failure> wait;
  const int window = window_ms > 0 ? window_ms : 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(window + 4000);
  RunPunchAsync(introducer_peer_key, target_peer_id, my_addrs, window_ms, reason,
                [wait](PunchRoe value) { wait.Finish(std::move(value)); });
  // Park outside mux — caller IoPump must drain PostToIo (MeshRuntime::Pump / harness PumpAll).
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Failure::Of(Err::Timeout, "punch: cold punch timed out"));
}

void PunchClientCoordinator::RunPunchAsync(const std::string& introducer_peer_key,
                                        const std::string& target_peer_id,
                                        const std::vector<std::string>& my_addrs, int window_ms,
                                        const std::string& reason, std::function<void(PunchRoe)> on_done) {
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = std::make_shared<std::function<void(PunchRoe)>>();
  *finish_once = [on_done = std::move(on_done), settled](PunchRoe value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (on_done) {
      on_done(std::move(value));
    }
  };

  if (!IsStarted()) {
    (*finish_once)(PunchRoe::error(Failure::Of(Err::NotStarted, "amp punch coordinator not started")));
    return;
  }
  if (!runtime_.Links().GetLinkSnapshot(introducer_peer_key).has_endpoint) {
    (*finish_once)(
        PunchRoe::error(Failure::Of(Err::EndpointNotRegistered, "introducer endpoint not registered")));
    return;
  }
  if (target_peer_id.empty()) {
    (*finish_once)(PunchRoe::error(Failure::Of(Err::InvalidRequest, "empty target_peer_id")));
    return;
  }
  const auto sanitized = SanitizePunchAddrs(my_addrs);
  if (sanitized.empty()) {
    (*finish_once)(PunchRoe::error(Failure::Of(Err::InvalidRequest, "no my_addrs")));
    return;
  }
  const int window = window_ms > 0 ? window_ms : 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(window + 4000);

  PunchConnectRequest req;
  req.target_peer_id = target_peer_id;
  req.addrs = sanitized;
  req.window_ms = window;
  req.reason = reason.empty() ? "cold" : reason;
  const std::string request_json = EncodePunchConnect(req);

  auto session = std::make_shared<pp::amp::ChannelSession>();
  auto finish = std::make_shared<std::function<void(PunchRoe)>>();
  *finish = [this, finish_once, session](PunchRoe value) {
    auto deliver = [finish_once, session, value = std::move(value)]() mutable {
      if (session) {
        session->Close();
      }
      (*finish_once)(std::move(value));
    };
    PostDeferred(std::move(deliver));
  };

  const auto read_timeout = PunchRemainingTimeout(deadline);
  runtime_.Links().EnsureAssociation(
      introducer_peer_key, [this, introducer_peer_key, target_peer_id, request_json, finish, session, deadline,
                            read_timeout, settled](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
        if (!assoc) {
          (*finish)(PunchRoe::error(WrapPunchLinkFailure(assoc.error())));
          return;
        }
        runtime_.Links().OpenChannel(
            introducer_peer_key, kAmpPunchProtocolId, PunchJsonChannelPolicy(read_timeout),
            [this, introducer_peer_key, target_peer_id, request_json, finish, session, deadline, read_timeout,
             settled](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
              if (!channel) {
                (*finish)(PunchRoe::error(WrapPunchLinkFailure(channel.error())));
                return;
              }
              AmpScheduleWhenChannelOpen(
                  [this](std::function<void()> fn) { PostStrand(std::move(fn)); }, io_pump_,
                  [this, introducer_peer_key, channel_id = *channel]() {
                    auto* link = runtime_.Links().FindLink(introducer_peer_key);
                    return link && link->Mux() &&
                           link->Mux()->State(channel_id) == pp::amp::ChannelState::Open;
                  },
                  deadline,
                  [this, introducer_peer_key, channel_id = *channel, target_peer_id, request_json, finish, session,
                   deadline, read_timeout, settled](bool open) mutable {
                    if (!open) {
                      (*finish)(PunchRoe::error(
                          Failure::Of(Err::ChannelFailed, "punch: introducer channel open failed")));
                      return;
                    }
                    auto* link = runtime_.Links().FindLink(introducer_peer_key);
                    if (!link || !link->Mux() ||
                        link->Mux()->State(channel_id) != pp::amp::ChannelState::Open) {
                      (*finish)(PunchRoe::error(
                          Failure::Of(Err::ChannelFailed, "punch: introducer channel open failed")));
                      return;
                    }
                    session->Bind(
                        *link->Mux(), channel_id, PunchJsonChannelPolicy(read_timeout),
                        [this, finish, target_peer_id](Roe<std::vector<uint8_t>> frame) {
                          if (!frame) {
                            (*finish)(PunchRoe::error(
                                Failure::Of(Err::ProtocolError, "punch: failed to read introducer frame")));
                            return false;
                          }
                          auto root = TryParseObject(std::string(frame->begin(), frame->end()));
                          if (!root) {
                            (*finish)(PunchRoe::error(
                                Failure::Of(Err::ProtocolError, "punch: invalid introducer frame")));
                            return false;
                          }
                          const std::string op = PunchOp(*root).value_or("");
                          if (op == "result") {
                            auto result = DecodePunchResult(*root);
                            if (!result) {
                              (*finish)(PunchRoe::error(
                                  Failure::Of(Err::ProtocolError, "punch: invalid result frame")));
                              return false;
                            }
                            if (result->ok) {
                              PublishPunchWinnerAddrs(runtime_.Links(), target_peer_id, result->winner_multiaddr);
                              (*finish)(*result);
                            } else {
                              (*finish)(PunchRoe::error(Failure::Of(
                                  Err::PunchFailed, result->error.empty() ? "punch failed" : result->error)));
                            }
                            return false;
                          }
                          if (op == "sync") {
                            auto sync = DecodePunchSync(*root);
                            if (!sync) {
                              (*finish)(PunchRoe::error(
                                  Failure::Of(Err::ProtocolError, "punch: invalid sync frame")));
                              return false;
                            }
                            auto run_burst = [this, finish, target_peer_id, sync = *sync]() mutable {
                              for (const std::string& ma : SanitizePunchAddrs(sync.peer_addrs)) {
                                if (auto parsed = pp::amp::ParseAdpMultiaddr(ma)) {
                                  if (!parsed->peer_id.empty()) {
                                    (void)runtime_.Links().RegisterEndpoint(parsed->peer_id, ma);
                                  }
                                }
                              }
                              auto apply = [this, finish, target_peer_id,
                                            epoch = sync.epoch_id](PunchBurstResult burst) {
                                PublishIfPunchConnected(runtime_.Links(), target_peer_id, burst);
                                PunchResult result;
                                result.epoch_id = epoch;
                                result.ok = burst.ok;
                                result.winner_multiaddr = burst.dialed;
                                result.error = burst.ok ? "" : burst.error;
                                if (burst.ok) {
                                  (*finish)(result);
                                } else {
                                  (*finish)(PunchRoe::error(Failure::Of(
                                      Err::PunchFailed,
                                      burst.error.empty() ? "punch burst failed" : burst.error)));
                                }
                              };
                              const int burst_window = sync.window_ms > 0 ? sync.window_ms : 2000;
                              runtime_.BurstDial(
                                  sync.peer_addrs, std::chrono::milliseconds(burst_window),
                                  [apply = std::move(apply)](pp::amp::BurstDialResult r) mutable {
                                    apply(ToPunchBurst(std::move(r)));
                                  });
                            };
                            // Leave mux before dial/burst start.
                            PostStrand(std::move(run_burst));
                            return true;
                          }
                          return true;
                        });
                    if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                      (*finish)(PunchRoe::error(Failure::Of(Err::ProtocolError, "punch: failed to send connect")));
                      return;
                    }
                    // Overall attempt deadline on Amp clock.
                    const auto attempt_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                    if (attempt_ms.count() > 0) {
                      runtime_.PostAfter(attempt_ms, [finish, settled]() {
                        if (!settled->load(std::memory_order_acquire)) {
                          (*finish)(PunchRoe::error(
                              Failure::Of(Err::Timeout, "punch: cold punch timed out")));
                        }
                      });
                    }
                  },
                  [this]() { return !IsStarted(); });
            });
      });
}

} // namespace pbr
