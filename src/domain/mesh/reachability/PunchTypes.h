#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

inline constexpr const char* kAmpPunchProtocolId = "/pp-browser/amp-punch/1.0.0";

enum class PunchRole {
  Unknown,
  Initiator,
  Introducer,
  Target,
};

enum class PunchPhase {
  Idle,
  Connecting,
  Exchanging,
  Syncing,
  Bursting,
  Connected,
  Failed,
};

struct PunchConnectRequest {
  std::string target_peer_id;
  std::vector<std::string> addrs;
  int window_ms = 2000;
  std::string reason = "cold";
};

struct PunchCandidates {
  std::string peer_id;
  std::vector<std::string> addrs;
  std::string nonce;
};

struct PunchSync {
  std::string epoch_id;
  std::vector<std::string> peer_addrs;
  int window_ms = 2000;
};

struct PunchResult {
  std::string epoch_id;
  bool ok = false;
  std::string winner_multiaddr;  // ADP multiaddr of winning path when ok
  std::string error;
};

} // namespace pbr
