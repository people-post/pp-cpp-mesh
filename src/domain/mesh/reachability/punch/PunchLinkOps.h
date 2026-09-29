#pragma once

#include "amp/L3/ChannelPolicy.h"
#include "amp/link/MeshRuntime.h"
#include "amp/link/PeerLinkManager.h"
#include "domain/mesh/reachability/punch/PunchBurst.h"
#include "common/CodedFailure.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/**
 * Pieces both ends of Amp coordinated punch share: the error type, the punch JSON channel policy,
 * and how a finished burst becomes a published winner address.
 * Errors follow docs/contracts/CODED_FAILURE.md — link failures are wrapped at this layer.
 */
enum class PunchErr : int32_t {
  Ok = 0,
  NotStarted,
  EndpointNotRegistered,
  InvalidRequest,
  LinkFailed,
  Timeout,
  ChannelFailed,
  ProtocolError,
  PunchFailed,
  Generic,
};

using PunchFailure = CodedFailure<PunchErr>;

PunchFailure WrapPunchLinkFailure(const pp::amp::PeerLinkManager::Failure& child);

/** Control JSON that stays open across connect / offer → sync. */
pp::amp::ChannelPolicy PunchJsonChannelPolicy(std::chrono::milliseconds read_timeout);

/** Time left until `deadline`, at least 1 ms. */
std::chrono::milliseconds PunchRemainingTimeout(std::chrono::steady_clock::time_point deadline);

PunchBurstResult ToPunchBurst(pp::amp::BurstDialResult r);

/**
 * After a burst: count an already-direct link as success and publish the winning address for
 * `known_peer_id` (or the PeerId in the dialed multiaddr).
 */
void PublishIfPunchConnected(pp::amp::PeerLinkManager& links, const std::string& known_peer_id,
                             PunchBurstResult& burst);

} // namespace pbr
