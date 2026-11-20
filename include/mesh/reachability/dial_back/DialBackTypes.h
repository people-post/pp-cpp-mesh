#pragma once

#include "amp/L1/Types.h"
#include "amp/link/PeerLinkManager.h"
#include "common/CodedFailure.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pp::mesh {

inline constexpr const char* kReachProtocolId = "/pp-mesh/reach/1.0.0";
inline constexpr const char* kDialBackProtocolId = kReachProtocolId;
/** The `v` a probe and its answer carry; a probe with a higher one is refused `bad_version` (docs/ERRORS.md). */
inline constexpr int kDialBackWireVersion = 1;

/**
 * Why a probe did not land: the `code` of an answer with `ok`:false (docs/ERRORS.md). The answer's `error` is text for
 * logs. An answer without a `code` (a v9.0.0 seed's) or with one this build does not know reads as `Generic`.
 */
enum class DialBackRefusal : int32_t {
  Ok = 0,
  /** Not JSON, or an op the seed does not serve. */
  BadRequest,
  /** A probe from this peer is already running there. */
  Busy,
  /** The probe ran: no target was allowed, or none answered. */
  NotReached,
  /** The probe's `v` is newer than the seed reads (the answer names `supported_versions`). */
  BadVersion,
  Generic,
};

inline const char* DialBackRefusalWire(const DialBackRefusal refusal) {
  switch (refusal) {
    case DialBackRefusal::BadRequest:
      return "bad_request";
    case DialBackRefusal::Busy:
      return "busy";
    case DialBackRefusal::NotReached:
      return "not_reached";
    case DialBackRefusal::BadVersion:
      return "bad_version";
    case DialBackRefusal::Ok:
    case DialBackRefusal::Generic:
      break;
  }
  return "";
}

/** A failed answer's refusal from its `code`; a missing or unknown code (an older or newer seed's) is `Generic`. */
inline DialBackRefusal DialBackRefusalFromCode(const std::string_view code) {
  for (const DialBackRefusal refusal :
       {DialBackRefusal::BadRequest, DialBackRefusal::Busy, DialBackRefusal::NotReached, DialBackRefusal::BadVersion}) {
    if (code == DialBackRefusalWire(refusal)) {
      return refusal;
    }
  }
  return DialBackRefusal::Generic;
}

struct DialBackProbeResult {
  bool ok = false;
  /** Target multiaddr the seed successfully dialed (inbound reachability chrome), if any. */
  std::string dialed;
  /**
   * Seed's view of the client's Amp UDP endpoint on the probe association (B26 reflexive).
   * Present even when dialing advertised LAN targets fails — required for cross-net IPv4 advertise.
   */
  std::string observed;
  /** Not ok: why, by the answer's `code` (`Generic` when it has none); `Ok` when ok. */
  DialBackRefusal refusal = DialBackRefusal::Ok;
  std::string error;
};

/** Errors follow pp-browser docs/contracts/CODED_FAILURE.md — link failures are wrapped at this layer. */
enum class DialBackErr : int32_t {
  Ok = 0,
  NotStarted,
  EndpointNotRegistered,
  InvalidRequest,
  LinkFailed,
  Timeout,
  ChannelFailed,
  ProtocolError,
  /** The seed does not read our probe's version (`bad_version`): it needs an update. */
  VersionMismatch,
  Generic,
};

using DialBackFailure = CodedFailure<DialBackErr>;

/** Map immediate link-manager failure → dial-back Err (never inspect ADP/PeerLink codes). */
DialBackFailure WrapDialBackLinkFailure(const pp::amp::PeerLinkManager::Failure& child);

/**
 * Whether a seed may dial `target` for a requester it observes at `observed` (port / scope
 * ignored — NAT commonly rewrites the port):
 * - same family, IPv4: the requester's own observed address;
 * - same family, IPv6: the requester's own /64 — a SLAAC temporary / privacy source address
 *   still vouches for the stable address the client advertises;
 * - across families (e.g. the global IPv6 targets reach ranks first, over an IPv4 link): only a
 *   public-routable target, never a private / loopback / link-local third party.
 * With the target cap, timeout clamp and one-probe-in-flight guard this keeps dial-back from
 * becoming an open "dial anywhere for anyone" relay.
 */
bool DialBackTargetAllowed(const pp::adp::IpEndpoint& target, const pp::adp::IpEndpoint& observed);

} // namespace pp::mesh
