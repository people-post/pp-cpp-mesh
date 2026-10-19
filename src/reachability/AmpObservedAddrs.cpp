#include "domain/mesh/reachability/AmpObservedAddrs.h"

#include "amp/link/AdpMultiaddr.h"

#include <algorithm>
#include <unordered_set>

namespace pbr {
namespace {

void AppendUnique(std::vector<std::string>& out, std::unordered_set<std::string>& seen,
                  const std::string& ma) {
  if (ma.empty() || !seen.insert(ma).second) {
    return;
  }
  out.push_back(ma);
}

} // namespace

bool IsUsableAdpListen(const std::string& ma) {
  if (ma.empty() || !pp::amp::ParseAdpMultiaddr(ma)) {
    return false;
  }
  const std::string host = IpHostFromMultiaddrPrefix(ma);
  if (host.empty() || host == "0.0.0.0" || host == "::" || host == "127.0.0.1" || host == "::1") {
    return false;
  }
  // Link-local / APIPA / virbr dogfood nets — never publish or dial (B13).
  if (IsLikelyUndialableLanIpv4(host)) {
    return false;
  }
  if (host.rfind("fe80:", 0) == 0 || host.rfind("FE80:", 0) == 0) {
    return false;
  }
  return true;
}

namespace {

std::vector<std::string> MergeAll(const AmpObservedAddrSet& set) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  for (const std::string& ma : set.listen) {
    if (IsUsableAdpListen(ma)) {
      AppendUnique(out, seen, ma);
    }
  }
  for (const std::string& ma : set.upnp) {
    if (IsUsableAdpListen(ma)) {
      AppendUnique(out, seen, ma);
    }
  }
  for (const std::string& ma : set.dial_back) {
    if (IsUsableAdpListen(ma)) {
      AppendUnique(out, seen, ma);
    }
  }
  return out;
}

} // namespace

std::vector<std::string> AmpObservedAddrSet::MergedForAdvertise() const {
  return RankAmpDialMultiaddrs(MergeAll(*this));
}

std::vector<std::string> AmpObservedAddrSet::MergedForPunch() const {
  return RankAmpDialMultiaddrs(MergeAll(*this));
}

AmpObservedAddrSet CollectAmpObservedAddrs(const std::string& amp_listen_multiaddr,
                                           const std::string& local_peer_id,
                                           const ReachabilitySnapshot& snapshot) {
  AmpObservedAddrSet out;
  // Prefer global /ip6 first (H002/N013); LAN private /ip4 follows for PreferLocal.
  out.listen = BuildAmpGlobalIpv6AdvertisedAddrs(amp_listen_multiaddr, local_peer_id);
  for (const std::string& ma : BuildAmpLanAdvertisedAddrs(amp_listen_multiaddr, local_peer_id)) {
    if (std::find(out.listen.begin(), out.listen.end(), ma) == out.listen.end()) {
      out.listen.push_back(ma);
    }
  }
  if (out.listen.empty() && IsUsableAdpListen(amp_listen_multiaddr)) {
    out.listen.push_back(amp_listen_multiaddr);
  }

  if (snapshot.signals.upnp_mapped && !snapshot.signals.upnp_external_ip.empty()) {
    const auto targets = BuildAmpReachabilityProbeTargets(
        amp_listen_multiaddr, local_peer_id, snapshot.signals.upnp_external_ip);
    for (const std::string& ma : targets) {
      const std::string host = IpHostFromMultiaddrPrefix(ma);
      if (host == snapshot.signals.upnp_external_ip && IsUsableAdpListen(ma)) {
        out.upnp.push_back(ma);
        break;
      }
    }
  }

  if (snapshot.signals.dial_back_ok && !snapshot.signals.dial_back_dialed.empty() &&
      IsUsableAdpListen(snapshot.signals.dial_back_dialed)) {
    out.dial_back.push_back(snapshot.signals.dial_back_dialed);
  }
  // B26: seed-observed reflexive endpoint is required for cross-net IPv4 when UPnP/global
  // listen are absent. Prefer it even when the seed could not dial our LAN advertise targets.
  if (!snapshot.signals.dial_back_observed.empty() &&
      IsUsableAdpListen(snapshot.signals.dial_back_observed)) {
    const std::string host = IpHostFromMultiaddrPrefix(snapshot.signals.dial_back_observed);
    const bool usable_public_v4 =
        snapshot.signals.dial_back_observed.rfind("/ip4/", 0) == 0 && IsPublicIpv4(host);
    const bool usable_global_v6 =
        snapshot.signals.dial_back_observed.rfind("/ip6/", 0) == 0 && IsGlobalIpv6(host);
    if (usable_public_v4 || usable_global_v6) {
      if (std::find(out.dial_back.begin(), out.dial_back.end(), snapshot.signals.dial_back_observed) ==
          out.dial_back.end()) {
        // Reflexive first so MergedForAdvertise ranks public before LAN.
        out.dial_back.insert(out.dial_back.begin(), snapshot.signals.dial_back_observed);
      }
    }
  }
  return out;
}

} // namespace pbr
