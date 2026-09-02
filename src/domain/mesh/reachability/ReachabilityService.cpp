#include "domain/mesh/reachability/ReachabilityService.h"

#include "amp/link/AdpMultiaddr.h"
#include "domain/mesh/reachability/AmpDialBackService.h"
#include "domain/mesh/reachability/NatTraversal.h"
#include "common/ValueJson.h"

#include <chrono>
#include <future>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

ReachabilitySignals AnalyzeAmpListen(const std::string& amp_listen,
                                     const std::vector<std::string>& /*ipv6_unused*/) {
  ReachabilitySignals signals;
  if (auto parsed = pp::amp::ParseAdpMultiaddr(amp_listen)) {
    const std::string ip = IpHostFromMultiaddrPrefix(amp_listen);
    signals.listen_is_wildcard = (ip == "0.0.0.0" || ip == "::");
    if (!ip.empty() && !signals.listen_is_wildcard) {
      signals.has_private_listen_ip = IsPrivateIpv4(ip);
      signals.has_public_listen_ip = IsPublicIpv4(ip);
    }
  } else {
    signals.listen_is_wildcard = amp_listen.find("/ip4/0.0.0.0/") != std::string::npos;
  }
  return signals;
}

std::optional<std::string> FirstAdpBootstrap(const std::vector<std::string>& bootstrap_peers) {
  for (const std::string& ma : bootstrap_peers) {
    if (pp::amp::ParseAdpMultiaddr(ma)) {
      return ma;
    }
  }
  return std::nullopt;
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

void ReachabilityService::StartProbe(AmpReachabilityProbeDeps deps) {
  if (probing_.exchange(true)) {
    return;
  }

  ReachabilitySnapshot checking;
  checking.status = ReachabilityStatus::Checking;
  Publish(checking);

  auto run = [this, deps = std::move(deps)]() mutable {
    RunProbe(std::move(deps));
    probing_.store(false);
  };
  if (deps.post_worker) {
    deps.post_worker(std::move(run));
  } else {
    run();
  }
}

void ReachabilityService::RunProbeBlocking(AmpReachabilityProbeDeps deps) {
  StartProbe(std::move(deps));
  while (probing_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void ReachabilityService::RunProbe(AmpReachabilityProbeDeps deps) {
  ReachabilitySnapshot result;
  result.measured_at = Clock::now();

  if (!deps.links || !deps.dial_back || deps.amp_listen_multiaddr.empty() || deps.local_peer_id.empty()) {
    result.status = ReachabilityStatus::Unknown;
    Publish(result);
    return;
  }

  result.signals = AnalyzeAmpListen(deps.amp_listen_multiaddr, {});

  const auto udp_port = UdpPortFromMultiaddr(deps.amp_listen_multiaddr);
  if (deps.try_upnp_first && udp_port && !ShouldSkipUpnpForListen(deps.amp_listen_multiaddr)) {
    auto mapped = TryUpnpUdpPortMapping(*udp_port);
    if (mapped.ok) {
      result.signals.upnp_mapped = true;
      result.signals.upnp_external_ip = mapped.external_ip;
      result.signals.upnp_external_port = mapped.external_port;
      upnp_external_ip_ = mapped.external_ip;
      upnp_external_port_ = mapped.external_port;
    }
  }

  auto seed_ma = FirstAdpBootstrap(deps.bootstrap_peers);
  if (!seed_ma) {
    result.signals.seed_dial_error = "no ADP bootstrap peers (dial-back needs /udp/…/adp/1.0.0/p2p/…)";
    // Without an Amp seed we cannot distinguish inbound; keep chrome honest (not Blocked).
    result.status = ReachabilityStatus::Unknown;
    Publish(result);
    return;
  }

  const std::string seed_key = "reachability:seed";
  if (auto registered = deps.links->RegisterEndpoint(seed_key, *seed_ma); !registered) {
    result.signals.seed_dial_error = registered.error().message;
    result.status = ClassifyReachability(result.signals);
    Publish(result);
    return;
  }

  {
    auto seed_promise = std::make_shared<std::promise<Roe<void>>>();
    auto seed_future = seed_promise->get_future();
    deps.links->EnsureAssociation(seed_key, [seed_promise](pp::amp::PeerLinkManager::LinkRoe dial_result) {
      try {
        if (dial_result) {
          seed_promise->set_value(Roe<void>());
        } else {
          seed_promise->set_value(Roe<void>(Error(dial_result.error().message)));
        }
      } catch (const std::future_error&) {
      }
    });
    const auto deadline = Clock::now() + std::chrono::milliseconds(10000);
    while (Clock::now() < deadline &&
           seed_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
      if (deps.io_pump) {
        deps.io_pump();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    if (seed_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      auto seed_result = seed_future.get();
      result.signals.seed_dial_ok = static_cast<bool>(seed_result);
      if (!seed_result) {
        result.signals.seed_dial_error = seed_result.error().message;
      }
    } else {
      result.signals.seed_dial_error = "seed dial timed out";
    }
  }

  if (result.signals.seed_dial_ok) {
    const auto targets = BuildAmpReachabilityProbeTargets(
        deps.amp_listen_multiaddr, deps.local_peer_id, upnp_external_ip_);
    if (deps.dial_back->IsStarted()) {
      auto probed = deps.dial_back->Probe(seed_key, targets, 8000);
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
