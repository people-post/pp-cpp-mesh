#pragma once

#include "domain/mesh/reachability/Reachability.h"

#include <string>
#include <vector>

namespace pbr {

/** True when `ma` is a parseable ADP multiaddr with a dialable host (not wildcard/loopback/link-local). */
bool IsUsableAdpListen(const std::string& ma);

/** Observed Amp ADP listen candidates used for ch0 advertise and punch. */
struct AmpObservedAddrSet {
  std::vector<std::string> listen;     // bind + LAN expansion
  std::vector<std::string> upnp;       // UPnP external mapping (if any)
  std::vector<std::string> dial_back;  // dialed target and/or seed-observed reflexive (B26)

  /** Ordered unique ADP multiaddrs for ch0 / SetLocalListenMultiaddrs. */
  std::vector<std::string> MergedForAdvertise() const;
  /** Same merge, intended for punch candidate exchange. */
  std::vector<std::string> MergedForPunch() const;
};

/**
 * Collect listen / UPnP / dial-back observed ADP multiaddrs.
 * Includes seed-observed reflexive public IPv4/IPv6 (B26) when dial-back probe reports it —
 * required for cross-net when LAN-only listen + no UPnP.
 */
AmpObservedAddrSet CollectAmpObservedAddrs(const std::string& amp_listen_multiaddr,
                                           const std::string& local_peer_id,
                                           const ReachabilitySnapshot& snapshot);

} // namespace pbr
