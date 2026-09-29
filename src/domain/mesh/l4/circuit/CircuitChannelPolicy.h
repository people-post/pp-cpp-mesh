#pragma once

#include "amp/L3/ChannelPolicy.h"
#include "amp/link/Types.h"
#include "domain/mesh/l4/shared/ProductChannelPolicies.h"

#include <string>

namespace pbr {

/** Channel policy for a circuit leg carrying `target_protocol` (both ends of a tunnel agree on it). */
inline pp::amp::ChannelPolicy CircuitTargetChannelPolicy(const std::string& target_protocol) {
  if (target_protocol == pp::amp::kAmpCircuitCarrierProtocolId) {
    return pp::amp::CircuitCarrierChannelPolicy();
  }
  return pp::amp::CircuitTunnelChannelPolicy();
}

} // namespace pbr
