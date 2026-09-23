#pragma once

#include <string>

namespace pbr {

inline constexpr const char* kReachProtocolId = "/pp-browser/reach/1.0.0";
inline constexpr const char* kDialBackProtocolId = kReachProtocolId;

struct DialBackProbeResult {
  bool ok = false;
  /** Target multiaddr the seed successfully dialed (inbound reachability chrome), if any. */
  std::string dialed;
  /**
   * Seed's view of the client's Amp UDP endpoint on the probe association (B26 reflexive).
   * Present even when dialing advertised LAN targets fails — required for cross-net IPv4 advertise.
   */
  std::string observed;
  std::string error;
};

} // namespace pbr
