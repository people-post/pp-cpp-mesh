#include "mesh/host/UdpSocketProbe.h"

#include <algorithm>

namespace pp::mesh {

adp::IpEndpoint UdpSocketProbeTarget(const adp::IpEndpoint& local) {
  const size_t n = local.family == adp::IpEndpoint::Family::V4 ? 4 : 16;
  const bool any = std::all_of(local.addr.begin(), local.addr.begin() + static_cast<std::ptrdiff_t>(n),
                               [](uint8_t b) { return b == 0; });
  if (!any) {
    return local;
  }
  if (local.family == adp::IpEndpoint::Family::V4) {
    return adp::IpEndpoint::V4(127, 0, 0, 1, local.port);
  }
  std::array<uint8_t, 16> loopback{};
  loopback[15] = 1;
  return adp::IpEndpoint::V6(loopback, local.port);
}

} // namespace pp::mesh
