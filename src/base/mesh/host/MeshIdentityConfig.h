#pragma once

#include <optional>
#include <vector>

namespace pbr {

/** Device ML-DSA identity for Amp PeerId (historically shared with Libp2pHost). */
struct MeshIdentityConfig {
  std::optional<std::vector<uint8_t>> device_ml_dsa_private_key;
  std::optional<std::vector<uint8_t>> device_ml_dsa_public_key;
};

} // namespace pbr
