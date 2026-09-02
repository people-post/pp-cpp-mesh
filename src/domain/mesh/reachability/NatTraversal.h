#pragma once

#include "common/Error.h"

#include <string>

namespace pbr {

struct UpnpMappingResult {
  bool ok = false;
  std::string external_ip;
  int external_port = 0;
  std::string error;
};

/** Try UPnP IGD UDP port mapping WAN→internal (Amp listen). */
UpnpMappingResult TryUpnpUdpPortMapping(int internal_port, int external_port = 0);

/** Remove a prior UPnP UDP mapping (best-effort). */
void ReleaseUpnpUdpPortMapping(int external_port);

} // namespace pbr
