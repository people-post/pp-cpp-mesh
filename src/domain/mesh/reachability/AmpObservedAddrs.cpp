#include "domain/mesh/reachability/AmpObservedAddrs.h"

#include "amp/link/AdpMultiaddr.h"

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

bool IsUsableAdpListen(const std::string& ma) {
  if (ma.empty() || !pp::amp::ParseAdpMultiaddr(ma)) {
    return false;
  }
  const std::string host = IpHostFromMultiaddrPrefix(ma);
  if (host.empty() || host == "0.0.0.0" || host == "::" || host == "127.0.0.1" || host == "::1") {
    return false;
  }
  return true;
}

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

std::vector<std::string> AmpObservedAddrSet::MergedForAdvertise() const { return MergeAll(*this); }

std::vector<std::string> AmpObservedAddrSet::MergedForPunch() const { return MergeAll(*this); }

AmpObservedAddrSet CollectAmpObservedAddrs(const std::string& amp_listen_multiaddr,
                                           const std::string& local_peer_id,
                                           const ReachabilitySnapshot& snapshot) {
  AmpObservedAddrSet out;
  out.listen = BuildAmpLanAdvertisedAddrs(amp_listen_multiaddr, local_peer_id);
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
  return out;
}

} // namespace pbr
