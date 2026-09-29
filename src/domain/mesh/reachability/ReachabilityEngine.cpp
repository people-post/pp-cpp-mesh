#include "domain/mesh/reachability/ReachabilityEngine.h"

#include "amp/link/AdpMultiaddr.h"
#include "domain/mesh/reachability/dial_back/client/DialBackClient.h"
#include "domain/mesh/reachability/NatTraversal.h"
#include "domain/mesh/reachability/ReachabilityNetIf.h"
#include "domain/mesh/shared/AmpParkUntil.h"
#include "common/ValueJson.h"
#include "foundation/runtime/AppRuntime.h"

#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;
/** Reachability belongs to connectivity (thread-ownership T001). */
constexpr OwnerThreadId kOwner = OwnerThreadId::Connectivity;

ReachabilitySignals AnalyzeAmpListen(const std::string& amp_listen,
                                     const std::vector<std::string>& ipv6_addrs) {
  ReachabilitySignals signals;
  if (auto parsed = pp::amp::ParseAdpMultiaddr(amp_listen)) {
    const std::string ip = IpHostFromMultiaddrPrefix(amp_listen);
    signals.listen_is_wildcard = (ip == "0.0.0.0" || ip == "::");
    if (!ip.empty() && !signals.listen_is_wildcard) {
      if (amp_listen.rfind("/ip6/", 0) == 0) {
        signals.has_global_ipv6 = IsGlobalIpv6(ip);
      } else {
        signals.has_private_listen_ip = IsPrivateIpv4(ip);
        signals.has_public_listen_ip = IsPublicIpv4(ip);
      }
    }
  } else {
    signals.listen_is_wildcard = amp_listen.find("/ip4/0.0.0.0/") != std::string::npos ||
                                 amp_listen.find("/ip6/::/") != std::string::npos;
  }
  for (const std::string& addr : ipv6_addrs) {
    if (IsGlobalIpv6(addr)) {
      signals.has_global_ipv6 = true;
      break;
    }
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

ReachabilitySnapshot ReachabilityEngine::Snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

void ReachabilityEngine::SetOnUpdated(std::function<void()> callback) {
  std::lock_guard lock(mutex_);
  on_updated_ = std::move(callback);
}

void ReachabilityEngine::Publish(ReachabilitySnapshot snapshot) {
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

ReachabilityEngine::~ReachabilityEngine() {
  AppRuntime::RunAndWait(kOwner, [this]() { alive_->store(false, std::memory_order_release); });
}

void ReachabilityEngine::StartProbe(AmpReachabilityProbeDeps deps) {
  if (probing_.exchange(true)) {
    return;
  }
  ReachabilitySnapshot checking;
  checking.status = ReachabilityStatus::Checking;
  AppRuntime::PostToOwnerOrRun(kOwner, [this, alive = alive_, checking, deps = std::move(deps)]() mutable {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    Publish(checking);
    RunProbe(std::move(deps));
  });
}

void ReachabilityEngine::RunProbeBlocking(AmpReachabilityProbeDeps deps) {
  auto io_pump = deps.io_pump;
  StartProbe(std::move(deps));
  while (probing_.load()) {
    if (io_pump) {
      io_pump();
    } else if (AppRuntime::OwnerThreadsManual()) {
      AppRuntime::RunOwnerTasks(kOwner);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

void ReachabilityEngine::Complete(ReachabilitySnapshot result, const bool classify) {
  AppRuntime::PostToOwnerOrRun(kOwner, [this, alive = alive_, result = std::move(result), classify]() mutable {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    if (classify) {
      result.status = ClassifyReachability(result.signals);
    }
    Publish(std::move(result));
    probing_.store(false);
  });
}

void ReachabilityEngine::RunProbe(AmpReachabilityProbeDeps deps) {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::Connectivity);
  ReachabilitySnapshot result;
  result.measured_at = Clock::now();

  if (!deps.links || !deps.dial_back || deps.amp_listen_multiaddr.empty() || deps.local_peer_id.empty()) {
    result.status = ReachabilityStatus::Unknown;
    Complete(std::move(result), /*classify=*/false);
    return;
  }

  result.signals = AnalyzeAmpListen(deps.amp_listen_multiaddr,
                                  reachability_netif::GlobalIpv6Addresses());

  const auto udp_port = UdpPortFromMultiaddr(deps.amp_listen_multiaddr);
  // N013: prefer IPv6 advertise over UPnP when a global address is already present.
  if (deps.try_upnp_first && udp_port && !ShouldSkipUpnpForListen(deps.amp_listen_multiaddr) &&
      !result.signals.has_global_ipv6) {
    // UPnP discovery blocks on a socket (~2 s): a worker step, then back onto the owner.
    auto upnp = [this, alive = alive_, port = *udp_port, deps, result]() mutable {
      auto mapped = TryUpnpUdpPortMapping(port);
      if (mapped.ok) {
        result.signals.upnp_mapped = true;
        result.signals.upnp_external_ip = mapped.external_ip;
        result.signals.upnp_external_port = mapped.external_port;
      }
      AppRuntime::PostToOwnerOrRun(kOwner, [this, alive, deps = std::move(deps), result = std::move(result)]() mutable {
        if (alive->load(std::memory_order_acquire)) {
          ProbeSeed(std::move(deps), std::move(result));
        }
      });
    };
    if (deps.post_worker) {
      auto post_worker = deps.post_worker;
      post_worker(std::move(upnp));
    } else {
      upnp();
    }
    return;
  }
  ProbeSeed(std::move(deps), std::move(result));
}

void ReachabilityEngine::ProbeSeed(AmpReachabilityProbeDeps deps, ReachabilitySnapshot result) {
  PBR_ASSERT_ON_OWNER(OwnerThreadId::Connectivity);
  if (result.signals.upnp_mapped) {
    upnp_external_ip_ = result.signals.upnp_external_ip;
    upnp_external_port_ = result.signals.upnp_external_port;
  }
  auto seed_ma = FirstAdpBootstrap(deps.bootstrap_peers);
  if (!seed_ma) {
    result.signals.seed_dial_error = "no ADP bootstrap peers (dial-back needs /udp/…/adp/1.0.0/p2p/…)";
    // Without an Amp seed we cannot distinguish inbound; keep chrome honest (not Blocked).
    result.status = ReachabilityStatus::Unknown;
    Complete(std::move(result), /*classify=*/false);
    return;
  }

  const std::string seed_key = "reachability:seed";
  if (auto registered = deps.links->RegisterEndpoint(seed_key, *seed_ma); !registered) {
    result.signals.seed_dial_error = registered.error().message;
    Complete(std::move(result), /*classify=*/true);
    return;
  }

  // Link work completes on Amp IO; the result hops back onto the owner (Complete).
  auto probe_finished = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = [this, probe_finished](ReachabilitySnapshot snap) {
    if (probe_finished->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    Complete(std::move(snap), /*classify=*/true);
  };

  // Settles when seed dial callback runs or seed dial times out (not when ProbeAsync ends).
  auto seed_settled = std::make_shared<std::atomic<bool>>(false);
  const auto seed_deadline = Clock::now() + std::chrono::milliseconds(10000);
  deps.links->EnsureAssociation(
      seed_key, [deps, result, seed_key, seed_settled, finish_once, upnp_ip = upnp_external_ip_](
                    pp::amp::PeerLinkManager::LinkRoe dial_result) mutable {
        if (seed_settled->exchange(true, std::memory_order_acq_rel)) {
          return;  // seed dial already timed out
        }
        if (!dial_result) {
          result.signals.seed_dial_ok = false;
          result.signals.seed_dial_error = dial_result.error().message;
          finish_once(std::move(result));
          return;
        }
        result.signals.seed_dial_ok = true;

        if (!deps.dial_back->IsStarted()) {
          result.signals.dial_back_error = "dial-back service not started";
          finish_once(std::move(result));
          return;
        }

        const auto targets =
            BuildAmpReachabilityProbeTargets(deps.amp_listen_multiaddr, deps.local_peer_id, upnp_ip);
        deps.dial_back->ProbeAsync(
            seed_key, targets,
            [result = std::move(result), finish_once](DialBackClient::ProbeRoe probed) mutable {
              if (probed) {
                result.signals.dial_back_ok = probed->ok;
                result.signals.dial_back_dialed = probed->dialed;
                result.signals.dial_back_observed = probed->observed;
                if (!probed->ok) {
                  result.signals.dial_back_error = probed->error;
                } else {
                  const std::string host = IpHostFromMultiaddrPrefix(probed->dialed);
                  if (probed->dialed.rfind("/ip6/", 0) == 0 && IsGlobalIpv6(host)) {
                    result.signals.has_global_ipv6 = true;
                  }
                }
                // B26: seed-observed reflexive counts as a public IPv4 listen signal for chrome.
                if (!probed->observed.empty()) {
                  const std::string oh = IpHostFromMultiaddrPrefix(probed->observed);
                  if (probed->observed.rfind("/ip4/", 0) == 0 && IsPublicIpv4(oh)) {
                    result.signals.has_public_listen_ip = true;
                  }
                  if (probed->observed.rfind("/ip6/", 0) == 0 && IsGlobalIpv6(oh)) {
                    result.signals.has_global_ipv6 = true;
                  }
                }
              } else {
                result.signals.dial_back_error = probed.error().message;
              }
              finish_once(std::move(result));
            },
            8000);
      });

  // Product: MeshPump + PostAfter — never park a thread on the seed dial.
  // Harness without post_after/post_io: AmpParkUntil + Tick until seed settles.
  AmpScheduleUntilSettled(
      deps.post_io, deps.io_pump, seed_settled, seed_deadline,
      [result, seed_settled, finish_once]() mutable {
        if (seed_settled->exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        result.signals.seed_dial_error = "seed dial timed out";
        finish_once(std::move(result));
      },
      deps.post_after);
}

std::string ReachabilityEngine::FormatOpsStatusJson() const {
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
  if (!snap.signals.dial_back_observed.empty()) {
    j.set("dial_back_observed", snap.signals.dial_back_observed);
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
