#pragma once

#include <optional>
#include <string>
#include <vector>

namespace pbr {

struct Libp2pHostConfig {
  std::string listen_multiaddr = "/ip4/0.0.0.0/tcp/18517";
  bool listen_enabled = true;
  std::optional<std::vector<uint8_t>> device_ml_dsa_private_key;
  std::optional<std::vector<uint8_t>> device_ml_dsa_public_key;
};

} // namespace pbr
