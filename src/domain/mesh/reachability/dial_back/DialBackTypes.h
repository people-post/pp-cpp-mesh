#pragma once

#include "amp/L1/Types.h"
#include "amp/link/PeerLinkManager.h"
#include "common/CodedFailure.h"

#include <cstdint>
#include <string>

namespace pbr {

inline constexpr const char* kReachProtocolId = "/pp-browser/reach/1.0.0";
inline constexpr const char* kDialBackProtocolId = kReachProtocolId;

struct DialBackProbeResult {
  bool ok = false;
  /** Target multiaddr the seed successfully dialed (inbound reachability chrome), if any. */
  std::string dialed;
  /**
   * Seed's view of the client's Amp UDP endpoint on the probe association (B26 reflexive).
   * Present even when dialing advertised LAN targets fails — required for cross-net IPv4 advertise.
   */
  std::string observed;
  std::string error;
};

/** Errors follow docs/contracts/CODED_FAILURE.md — link failures are wrapped at this layer. */
enum class DialBackErr : int32_t {
  Ok = 0,
  NotStarted,
  EndpointNotRegistered,
  InvalidRequest,
  LinkFailed,
  Timeout,
  ChannelFailed,
  ProtocolError,
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

} // namespace pbr
