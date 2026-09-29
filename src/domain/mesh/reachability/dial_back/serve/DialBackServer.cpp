#include "domain/mesh/reachability/dial_back/serve/DialBackServer.h"

#include "domain/mesh/l4/shared/InboundReply.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/AdpMultiaddr.h"
#include "common/ValueJson.h"
#include "foundation/runtime/DeferredSelf.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

/** One inbound probe's walk over its targets (IO strand only). */
struct DialTargetsWalk {
  pp::amp::MeshRuntime* runtime = nullptr;
  std::vector<std::string> targets;
  size_t next = 0;
  std::chrono::milliseconds timeout{8000};
  DialBackProbeResult result;
  std::function<void(DialBackProbeResult)> done;
};

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
    if (!pp::amp::ParseAdpMultiaddr(ma)) {
      out.error = "target is not an ADP multiaddr";
      out.dialed = ma;
      continue;
    }
    const std::string key = "dialback:probe:" + std::to_string(i);
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

/** IO strand: dial `targets` in order; `done` runs once, on IO. */
void DialAmpTargetsAsync(pp::amp::MeshRuntime& runtime, std::vector<std::string> targets, const int timeout_ms,
                         std::function<void(DialBackProbeResult)> done) {
  auto walk = std::make_shared<DialTargetsWalk>();
  walk->runtime = &runtime;
  walk->targets = std::move(targets);
  walk->timeout = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);
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
   * B26: the seed's view of the client's Amp UDP endpoint on this association. Dialing the
   * client's LAN advertise addrs often fails cross-NAT; the observed reflexive address is what
   * peers need to dial. IO strand only (link state is IO-affine).
   */
  std::string ObservedMultiaddrOnIo(const std::string& remote_peer_id) {
    if (auto* link = Links().FindLink(remote_peer_id)) {
      if (auto* conn = link->ConnectionOrNull()) {
        const auto ep = conn->PeerEndpoint();
        if (ep.port != 0) {
          if (auto ma = pp::amp::FormatAdpMultiaddr(ep, remote_peer_id)) {
            return *ma;
          }
        }
      }
    }
    return {};
  }

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
  void ServeProbe(std::shared_ptr<InboundReply> reply, std::string observed, std::vector<uint8_t> body) {
    DialBackProbeResult result;
    result.observed = std::move(observed);
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
    std::vector<std::string> targets;
    if (const Array* addrs = root->getArray("target_multiaddrs")) {
      for (const auto& item : addrs->elements) {
        if (auto s = asString(item)) {
          targets.push_back(*s);
        }
      }
    }
    const int timeout_ms = static_cast<int>(root->getNonNegInt("timeout_ms").value_or(8000));
    runtime->PostToIo(deferred.Bind([this, reply, observed = result.observed, targets = std::move(targets),
                                     timeout_ms]() mutable {
      if (stopped.load(std::memory_order_acquire) || !runtime) {
        return;
      }
      DialAmpTargetsAsync(*runtime, std::move(targets), timeout_ms,
                          [reply, observed](DialBackProbeResult dialed) {
                            dialed.observed = observed;
                            SendProbeResult(reply, dialed);
                          });
    }));
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
          ServeProbe(MakeInboundReply(session, IoPost()), ObservedMultiaddrOnIo(remote_peer_id),
                     std::move(*frame));
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
