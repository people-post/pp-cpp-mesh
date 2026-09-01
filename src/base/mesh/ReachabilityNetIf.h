#pragma once

#include <string>
#include <vector>

namespace pbr::reachability_netif {

/** Global (non-link-local, non-ULA, non-loopback) IPv6 addresses. Empty on Win32. */
std::vector<std::string> GlobalIpv6Addresses();

/** Public (non-RFC1918) IPv4 addresses on local interfaces. Empty on Win32. */
std::vector<std::string> PublicIpv4Addresses();

struct LanIpv4If {
  std::string ip;
  std::string ifname;
  bool up_running_non_loopback = false;
};

/** IPv4 unicast addresses with interface metadata for call-scoped LAN advertise. */
std::vector<LanIpv4If> LanIpv4Interfaces();

} // namespace pbr::reachability_netif
