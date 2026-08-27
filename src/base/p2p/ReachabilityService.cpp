#include "base/p2p/ReachabilityService.h"

#include "base/data/Libp2pRole.h"
#include "base/p2p/DialBackService.h"
#include "base/p2p/Libp2pWorker.h"
#include "base/p2p/NatTraversal.h"
#include "base/p2p/NodeRuntime.h"
#include "common/ValueJson.h"

#include <future>
#include <memory>

namespace pbr {

namespace {

ReachabilitySignals AnalyzeListenAddrs(const std::string& bound_listen, const std::vector<std::string>& ipv6_addrs) {
  ReachabilitySignals signals;
  const auto tcp_pos = bound_listen.find("/tcp/");
  const std::string prefix = tcp_pos == std::string::npos ? bound_listen : bound_listen.substr(0, tcp_pos);
  const std::string ip = IpHostFromMultiaddrPrefix(prefix);
  signals.listen_is_wildcard = (ip == "0.0.0.0" || ip == "::");
  if (!ip.empty() && !signals.listen_is_wildcard) {
    if (bound_listen.rfind("/ip6/", 0) == 0) {
      signals.has_global_ipv6 = IsGlobalIpv6(ip);
    } else {
      signals.has_private_listen_ip = IsPrivateIpv4(ip);
      signals.has_public_listen_ip = IsPublicIpv4(ip);
    }
  }
  signals.has_global_ipv6 = signals.has_global_ipv6 || !ipv6_addrs.empty();
  return signals;
}

} // namespace

ReachabilitySnapshot ReachabilityService::Snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

void ReachabilityService::SetOnUpdated(std::function<void()> callback) {
  std::lock_guard lock(mutex_);
  on_updated_ = std::move(callback);
}

void ReachabilityService::Publish(ReachabilitySnapshot snapshot) {
  std::function<void()> callback;
  {
    std::lock_guard lock(mutex_);
    snapshot_ = std::move(snapshot);
    callback = on_updated_;
  }
  if (callback) {
    callback();
  }
}

void ReachabilityService::StartProbe(NodeRuntime& runtime, DialBackService& dial_back, bool try_upnp_first) {
  if (probing_.exchange(true)) {
    return;
  }

  ReachabilitySnapshot checking;
  checking.status = ReachabilityStatus::Checking;
  Publish(checking);

  Libp2pHost* host = runtime.Host();
  if (!host) {
    probing_.store(false);
    return;
  }
  PostLibp2pWorker(*host, WorkerLane::Background, [this, &runtime, &dial_back, try_upnp_first]() {
    RunProbe(runtime, dial_back, try_upnp_first);
    probing_.store(false);
  });
}

void ReachabilityService::RunProbeBlocking(NodeRuntime& runtime, DialBackService& dial_back,
                                           bool try_upnp_first) {
  StartProbe(runtime, dial_back, try_upnp_first);
  while (probing_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void ReachabilityService::RunProbe(NodeRuntime& runtime, DialBackService& dial_back, bool try_upnp_first) {
  ReachabilitySnapshot result;
  result.measured_at = std::chrono::steady_clock::now();

  if (!runtime.IsRunning() || !runtime.Host() || !runtime.Sessions()) {
    result.status = ReachabilityStatus::Unknown;
    Publish(result);
    return;
  }

  const std::string bound = runtime.BoundListenMultiaddr();
  const auto ipv6_addrs = EnumerateGlobalIpv6Addresses();
  result.signals = AnalyzeListenAddrs(bound, ipv6_addrs);

  const auto port = TcpPortFromMultiaddr(bound);
  if (try_upnp_first && port && !ShouldSkipUpnpForListen(bound)) {
    auto mapped = TryUpnpTcpPortMapping(*port);
    if (mapped.ok) {
      result.signals.upnp_mapped = true;
      result.signals.upnp_external_ip = mapped.external_ip;
      result.signals.upnp_external_port = mapped.external_port;
      upnp_external_ip_ = mapped.external_ip;
      upnp_external_port_ = mapped.external_port;
    }
  }

  const std::string seed = PeerIdFromMultiaddr(kDefaultLibp2pBootstrapPeer);

  auto seed_promise = std::make_shared<std::promise<Roe<void>>>();
  auto seed_future = seed_promise->get_future();
  runtime.Sessions()->EnsureConnection(seed, [seed_promise](Roe<void> dial_result) {
    try {
      seed_promise->set_value(std::move(dial_result));
    } catch (const std::future_error&) {
    }
  });
  if (seed_future.wait_for(std::chrono::milliseconds(10000)) == std::future_status::ready) {
    auto seed_result = seed_future.get();
    result.signals.seed_dial_ok = static_cast<bool>(seed_result);
    if (!seed_result) {
      result.signals.seed_dial_error = seed_result.error().message;
    }
  } else {
    result.signals.seed_dial_error = "seed dial timed out";
  }

  if (result.signals.seed_dial_ok) {
    std::string peer_id;
    if (auto local = runtime.Host()->LocalPeerIdBase58()) {
      peer_id = *local;
    }
    const auto targets = BuildReachabilityProbeTargets(bound, peer_id, ipv6_addrs, upnp_external_ip_);
    if (dial_back.IsStarted()) {
      auto probed = dial_back.Probe(seed, targets, 8000);
      if (probed) {
        result.signals.dial_back_ok = probed->ok;
        result.signals.dial_back_dialed = probed->dialed;
        if (!probed->ok) {
          result.signals.dial_back_error = probed->error;
        }
      } else {
        result.signals.dial_back_error = probed.error().message;
      }
    } else {
      result.signals.dial_back_error = "dial-back service not started";
    }
  }

  result.status = ClassifyReachability(result.signals);
  Publish(result);
}

std::string ReachabilityService::FormatOpsStatusJson() const {
  const ReachabilitySnapshot snap = Snapshot();
  Object j;
  j.set("status", ReachabilityStatusKey(snap.status));
  j.set("seed_dial_ok", snap.signals.seed_dial_ok);
  j.set("dial_back_ok", snap.signals.dial_back_ok);
  j.set("upnp_mapped", snap.signals.upnp_mapped);
  j.set("has_global_ipv6", snap.signals.has_global_ipv6);
  if (!snap.signals.dial_back_dialed.empty()) {
    j.set("dial_back_dialed", snap.signals.dial_back_dialed);
  }
  if (!snap.signals.upnp_external_ip.empty()) {
    j.set("upnp_external_ip", snap.signals.upnp_external_ip);
  }
  if (!snap.signals.seed_dial_error.empty()) {
    j.set("seed_dial_error", snap.signals.seed_dial_error);
  }
  if (!snap.signals.dial_back_error.empty()) {
    j.set("dial_back_error", snap.signals.dial_back_error);
  }
  return DumpJson(j);
}

} // namespace pbr
