#pragma once

#include "amp/link/LinkEvents.h"
#include "amp/link/MeshRuntime.h"

#include <string>

namespace pbr {

/** `ip:port` / `[v6]:port` for log lines (not a multiaddr). */
std::string FormatLinkEndpointForLog(const pp::adp::IpEndpoint& endpoint);

/** One log line body for an Amp link event (without logger prefix). */
std::string FormatLinkEventForLog(const pp::amp::LinkEvent& event);

/**
 * Log every Amp link event under the `MeshLink` logger (call-path-resilience k0):
 * Connected / PathChanged at INFO, Dropped of a connected link at WARNING, failed
 * dial attempts at DEBUG (punch bursts create many). Listener lives as long as `runtime`.
 */
void InstallMeshLinkEventLog(pp::amp::MeshRuntime& runtime);

} // namespace pbr
