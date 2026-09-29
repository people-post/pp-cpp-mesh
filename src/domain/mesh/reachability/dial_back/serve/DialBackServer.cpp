#include "domain/mesh/reachability/dial_back/serve/DialBackServer.h"

#include "domain/mesh/l4/shared/InboundReply.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/AdpMultiaddr.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "foundation/runtime/DeferredSelf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

/** Any peer can ask us to dial `target_multiaddrs` — bound the blast radius. */
constexpr size_t kMaxDialBackTargets = 4;
constexpr int kMaxDialBackTimeoutMs = 15000;
constexpr int kMinDialBackTimeoutMs = 1000;

/** One inbound probe's walk over its targets (IO strand only). */
struct DialTargetsWalk {
  pp::amp::MeshRuntime* runtime = nullptr;
  std::vector<std::string> targets;
  size_t next = 0;
  std::chrono::milliseconds timeout{8000};
  DialBackProbeResult result;
  std::function<void(DialBackProbeResult)> done;
  /**
   * The requesting peer, used to build a small, reused set of RegisterEndpoint keys (one per
   * target slot) instead of a fresh key per probe, which would grow PeerLinkManager's endpoint
   * table forever. Only one probe is in flight per remote_peer_id at a time (see
   * TryMarkProbeInflight), so key reuse across a peer's successive probes is safe.
   */
  std::string remote_peer_id;
  /**
   * The requester's own authenticated reflexive endpoint (from the link, not attacker-suppliable).
   * Empty only when the link exposes no usable endpoint; in that case targets cannot be checked
   * and are rejected — dial-back must not become an open "dial anywhere for anyone" relay.
   */
  std::optional<pp::adp::IpEndpoint> observed_host;
};

/**
 * Per-peer in-flight probe tracking, held via shared_ptr and independent of Impl's lifetime: a
 * queued IO callback keeps this alive and can always clear its own entry, even if Impl itself
 * was freed (deferred.Bind made the rest of the callback a no-op) before the callback ran.
 */
struct InflightProbes {
  std::mutex mutex;
  std::set<std::string> peers;
};

/** True if `remote_peer_id` had no probe already in flight (and it is now marked in-flight). */
bool TryMarkProbeInflight(InflightProbes& state, const std::string& remote_peer_id) {
  std::lock_guard lock(state.mutex);
  return state.peers.insert(remote_peer_id).second;
}

void ClearProbeInflight(InflightProbes& state, const std::string& remote_peer_id) {
  std::lock_guard lock(state.mutex);
  state.peers.erase(remote_peer_id);
}

/** True when two endpoints are the same host (port/scope ignored — NAT commonly rewrites the port). */
bool SameHost(const pp::adp::IpEndpoint& a, const pp::adp::IpEndpoint& b) {
  if (a.family != b.family) {
    return false;
  }
  const size_t n = a.family == pp::adp::IpEndpoint::Family::V4 ? 4 : 16;
  return std::memcmp(a.addr.data(), b.addr.data(), n) == 0;
}

/**
 * Dial the next usable target: association or its deadline, whichever settles first, then the next
 * target — the first association that lands answers the probe. Callbacks on the IO strand; nothing
 * waits on a thread.
 */
void DialNextTarget(std::shared_ptr<DialTargetsWalk> walk) {
  DialBackProbeResult& out = walk->result;
  pp::amp::PeerLinkManager& links = walk->runtime->Links();
  while (walk->next < walk->targets.size()) {
    const size_t i = walk->next++;
    const std::string ma = walk->targets[i];
    if (ma.empty()) {
      continue;
    }
    auto parsed_target = pp::amp::ParseAdpMultiaddr(ma);
    if (!parsed_target) {
      out.error = "target is not an ADP multiaddr";
      out.dialed = ma;
      continue;
    }
    // Only ever dial back to the requester's own observed host: otherwise any peer could turn
    // this node into an open probe/relay against arbitrary third-party addresses.
    if (!walk->observed_host || !SameHost(parsed_target->endpoint, *walk->observed_host)) {
      out.error = "target is not the requester's observed host";
      out.dialed = ma;
      continue;
    }
    const std::string key = "dialback:probe:" + walk->remote_peer_id + ":" + std::to_string(i);
    if (auto registered = links.RegisterEndpoint(key, ma); !registered) {
      out.error = registered.error().message;
      out.dialed = ma;
      continue;
    }
    auto settled = std::make_shared<bool>(false);  // IO strand only
    auto finish = [walk, settled, ma](Roe<void> dialed) {
      if (std::exchange(*settled, true)) {
        return;
      }
      walk->result.dialed = ma;
      if (dialed) {
        walk->result.ok = true;
        walk->result.error.clear();
        walk->done(std::move(walk->result));
        return;
      }
      walk->result.error = dialed.error().message;
      DialNextTarget(walk);
    };
    links.EnsureAssociation(key, [finish](pp::amp::PeerLinkManager::LinkRoe linked) {
      finish(linked ? Roe<void>() : Roe<void>(Error(WrapDialBackLinkFailure(linked.error()).message)));
    });
    walk->runtime->PostAfter(walk->timeout, [finish]() { finish(Error("dial-back timed out")); });
    return;
  }
  walk->done(std::move(out));
}

/** IO strand: dial `targets` (already capped/host-filtered by the caller) in order; `done` runs once, on IO. */
void DialAmpTargetsAsync(pp::amp::MeshRuntime& runtime, std::vector<std::string> targets, const int timeout_ms,
                         std::string remote_peer_id, std::optional<pp::adp::IpEndpoint> observed_host,
                         std::function<void(DialBackProbeResult)> done) {
  auto walk = std::make_shared<DialTargetsWalk>();
  walk->runtime = &runtime;
  walk->targets = std::move(targets);
  const int clamped_timeout = std::clamp(timeout_ms > 0 ? timeout_ms : 8000, kMinDialBackTimeoutMs, kMaxDialBackTimeoutMs);
  walk->timeout = std::chrono::milliseconds(clamped_timeout);
  walk->remote_peer_id = std::move(remote_peer_id);
  walk->observed_host = observed_host;
  walk->done = std::move(done);
  if (walk->targets.empty()) {
    walk->result.error = "no target_multiaddrs";
    walk->done(std::move(walk->result));
    return;
  }
  DialNextTarget(std::move(walk));
}

} // namespace

struct DialBackServer::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  std::atomic<bool> stopped{false};
  /** Guards protocol-handler raw Impl* past Stop — OWNERSHIP.md § DeferredSelf. */
  DeferredSelf deferred;

  pp::amp::PeerLinkManager& Links() { return runtime->Links(); }
  /** IO lane for InboundReply. */
  InboundReply::IoPost IoPost() {
    return [rt = runtime](std::function<void()> task) { rt->PostToIo(std::move(task)); };
  }

  /**
   * B26: the seed's view of the client's Amp UDP endpoint on this association — comes from the
   * authenticated connection, not anything the peer put in the request, so it also doubles as
   * the only host `target_multiaddrs` are allowed to name (see SameHost). IO strand only (link
   * state is IO-affine).
   */
  std::optional<pp::adp::IpEndpoint> ObservedEndpointOnIo(const std::string& remote_peer_id) {
    if (auto* link = Links().FindLink(remote_peer_id)) {
      if (auto* conn = link->ConnectionOrNull()) {
        const auto ep = conn->PeerEndpoint();
        if (ep.port != 0) {
          return ep;
        }
      }
    }
    return std::nullopt;
  }

  std::string ObservedMultiaddrOnIo(const std::string& remote_peer_id) {
    if (auto ep = ObservedEndpointOnIo(remote_peer_id)) {
      if (auto ma = pp::amp::FormatAdpMultiaddr(*ep, remote_peer_id)) {
        return *ma;
      }
    }
    return {};
  }

  /** Coarse per-peer throttle: reject a probe while the same peer's previous one is still running. */
  std::shared_ptr<InflightProbes> inflight = std::make_shared<InflightProbes>();

  static void SendProbeResult(const std::shared_ptr<InboundReply>& reply, const DialBackProbeResult& result) {
    Object response;
    response.set("v", int64_t{1});
    response.set("ok", result.ok);
    response.set("dialed", result.dialed);
    response.set("observed", result.observed);
    response.set("error", result.error);
    reply->Send(JsonToBody(DumpJson(response)));
  }

  /** Frame handler (IO): parse, then walk the targets from a fresh IO task (mux stack unwound). */
  void ServeProbe(std::shared_ptr<InboundReply> reply, const std::string& remote_peer_id,
                  std::vector<uint8_t> body) {
    DialBackProbeResult result;
    result.observed = ObservedMultiaddrOnIo(remote_peer_id);
    const std::string json_utf8(body.begin(), body.end());
    auto root = TryParseObject(json_utf8);
    if (!root) {
      result.error = "invalid dial-back json";
      SendProbeResult(reply, result);
      return;
    }
    if (root->getString("op").value_or("") != "probe") {
      result.error = "unsupported op";
      SendProbeResult(reply, result);
      return;
    }
    // One in-flight probe per requesting peer: a peer that wants to flood dial attempts has to
    // do it serially (each probe already carries its own target-count/timeout caps).
    if (!TryMarkProbeInflight(*inflight, remote_peer_id)) {
      result.error = "dial-back probe already in flight for this peer";
      SendProbeResult(reply, result);
      return;
    }
    auto observed_host = ObservedEndpointOnIo(remote_peer_id);
    // Filter to the requester's own observed host first, then cap the count: capping before
    // filtering could let a requester pad the request with kMaxDialBackTargets non-matching
    // decoys ahead of its one real (matching) target and have that legitimate target dropped.
    std::vector<std::string> targets;
    if (const Array* addrs = root->getArray("target_multiaddrs")) {
      for (const auto& item : addrs->elements) {
        if (targets.size() >= kMaxDialBackTargets) {
          break;
        }
        auto s = asString(item);
        if (!s) {
          continue;
        }
        auto parsed_target = pp::amp::ParseAdpMultiaddr(*s);
        if (!parsed_target || !observed_host || !SameHost(parsed_target->endpoint, *observed_host)) {
          continue;
        }
        targets.push_back(*s);
      }
    }
    const int timeout_ms = static_cast<int>(root->getNonNegInt("timeout_ms").value_or(8000));
    // `inflight` is captured by shared_ptr (not through `this`), so its entry can always be
    // cleared below even on the path where `this` (Impl) is no longer safe to touch — a plain
    // deferred.Bind would silently no-op the whole callback on that path and leak the entry.
    const DeferredSelf::Token life_token = deferred.token();
    const uint64_t life_snap = deferred.Snapshot();
    runtime->PostToIo([this, reply, observed = result.observed, targets = std::move(targets), timeout_ms,
                       remote_peer_id, observed_host, life_token, life_snap, inflight = inflight]() mutable {
      if (!DeferredSelf::Alive(life_token, life_snap) || stopped.load(std::memory_order_acquire) || !runtime) {
        ClearProbeInflight(*inflight, remote_peer_id);
        return;
      }
      DialAmpTargetsAsync(*runtime, std::move(targets), timeout_ms, remote_peer_id, observed_host,
                          [reply, observed, remote_peer_id, inflight](DialBackProbeResult dialed) {
                            ClearProbeInflight(*inflight, remote_peer_id);
                            dialed.observed = observed;
                            SendProbeResult(reply, dialed);
                          });
    });
  }

  void HandleInboundOnLink(pp::amp::LinkHandle /*handle*/, const std::string& remote_peer_id,
                           const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime || remote_peer_id.empty()) {
      return;
    }
    auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    *session_holder = Links().BindChannel(
        remote_peer_id, channel_id, InboundReplyPolicy(pp::amp::ControlJsonChannelPolicy()),
        [this, session_holder, remote_peer_id](Roe<std::vector<uint8_t>> frame) {
          auto session = *session_holder;
          if (!session || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          // Keep the channel open for the worker's reply (InboundReply.h); `reply` closes it.
          ServeProbe(MakeInboundReply(session, IoPost()), remote_peer_id, std::move(*frame));
          return true;
        });
  }
};

DialBackServer::DialBackServer(pp::amp::MeshRuntime& runtime) : impl_(std::make_unique<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
}

DialBackServer::~DialBackServer() { Stop(); }

void DialBackServer::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  runtime_.Links().SetProtocolHandler(
      kDialBackProtocolId,
      impl_->deferred.Bind([impl = impl_.get()](pp::amp::LinkHandle handle, const std::string& remote_peer_id,
                                                const uint32_t channel_id) {
        impl->HandleInboundOnLink(handle, remote_peer_id, channel_id);
      }));
}

void DialBackServer::Stop() {
  // Idempotent: the destructor Stops again, possibly after MeshHost::Stop freed the runtime.
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.Links().RemoveProtocolHandler(kDialBackProtocolId);
  impl_->deferred.Invalidate();
}

} // namespace pbr
