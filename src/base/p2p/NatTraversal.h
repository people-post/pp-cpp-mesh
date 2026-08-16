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

/** Try UPnP IGD port mapping WAN→internal (nu). No-op when `ShouldSkipUpnpForListen`. */
UpnpMappingResult TryUpnpTcpPortMapping(int internal_port, int external_port = 0);

/** Remove a prior UPnP mapping (best-effort). */
void ReleaseUpnpTcpPortMapping(int external_port);

} // namespace pbr
