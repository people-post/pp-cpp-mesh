#pragma once

#include "domain/mesh/reachability/Reachability.h"

#include <string>
#include <vector>

namespace pbr {

/** Observed Amp ADP listen candidates used for ch0 advertise and punch. */
struct AmpObservedAddrSet {
  std::vector<std::string> listen;     // bind + LAN expansion
  std::vector<std::string> upnp;       // UPnP external mapping (if any)
  std::vector<std::string> dial_back;  // successful dial-back dialed addr

  /** Ordered unique ADP multiaddrs for ch0 / SetLocalListenMultiaddrs. */
  std::vector<std::string> MergedForAdvertise() const;
  /** Same merge, intended for punch candidate exchange. */
  std::vector<std::string> MergedForPunch() const;
};

/**
 * Collect listen / UPnP / dial-back observed ADP multiaddrs.
 * Skips wildcards and loopback; requires parseable ADP multiaddrs when validating.
 */
AmpObservedAddrSet CollectAmpObservedAddrs(const std::string& amp_listen_multiaddr,
                                           const std::string& local_peer_id,
                                           const ReachabilitySnapshot& snapshot);

} // namespace pbr
