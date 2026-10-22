#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pp::mesh {

/** Reach-kind punch SM — distinct id from dial-back (`kReachProtocolId`). */
inline constexpr const char* kAmpPunchProtocolId = "/pp-mesh/reach/punch/1.0.0";

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

/** Introducer → target: initiator identity + observed addrs + epoch. */
struct PunchOffer {
  std::string initiator_peer_id;
  std::vector<std::string> addrs;
  std::string epoch_id;
  int window_ms = 2000;
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

/** Why a punch did not run or did not win: the `code` of a failed result (docs/ERRORS.md). */
enum class PunchRefusal : int32_t {
  Ok = 0,
  /** The introducer does not know, or cannot reach, the target. */
  TargetUnknown,
  /** The target declines this initiator (its direct-connections audience). */
  Declined,
  NoCandidates,
  SendFailed,
  BadRequest,
  /** The burst ran and no path won. */
  NoPath,
  Generic,
};

inline const char* PunchRefusalWire(const PunchRefusal refusal) {
  switch (refusal) {
  case PunchRefusal::TargetUnknown:
    return "target_unknown";
  case PunchRefusal::Declined:
    return "declined";
  case PunchRefusal::NoCandidates:
    return "no_candidates";
  case PunchRefusal::SendFailed:
    return "send_failed";
  case PunchRefusal::BadRequest:
    return "bad_request";
  case PunchRefusal::NoPath:
    return "no_path";
  case PunchRefusal::Ok:
  case PunchRefusal::Generic:
    break;
  }
  return "";
}

/** A failed result's refusal. Peers before the `code` field sent text only: it is read here, and nowhere else. */
inline PunchRefusal PunchRefusalFromResult(const std::string_view code, const std::string_view legacy_text) {
  for (const PunchRefusal refusal : {PunchRefusal::TargetUnknown, PunchRefusal::Declined, PunchRefusal::NoCandidates,
                                     PunchRefusal::SendFailed, PunchRefusal::BadRequest, PunchRefusal::NoPath}) {
    if (!code.empty() && code == PunchRefusalWire(refusal)) {
      return refusal;
    }
  }
  if (!code.empty()) {
    return PunchRefusal::Generic;  // a code from a newer peer
  }
  const auto has = [legacy_text](const char* needle) { return legacy_text.find(needle) != std::string_view::npos; };
  if (has("declines")) {
    return PunchRefusal::Declined;
  }
  if (has("introducer") || has("not registered")) {
    return PunchRefusal::TargetUnknown;
  }
  if (has("no candidates")) {
    return PunchRefusal::NoCandidates;
  }
  if (has("invalid")) {
    return PunchRefusal::BadRequest;
  }
  if (has("failed to send")) {
    return PunchRefusal::SendFailed;
  }
  return PunchRefusal::Generic;
}

struct PunchResult {
  std::string epoch_id;
  bool ok = false;
  std::string winner_multiaddr;  // ADP multiaddr of winning path when ok
  /** Not ok: why (its wire `code`), and text for logs. */
  PunchRefusal refusal = PunchRefusal::Ok;
  std::string error;
};

} // namespace pp::mesh
