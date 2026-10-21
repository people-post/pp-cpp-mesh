#pragma once

#include <cstdint>

namespace pp::mesh {

/** Why one bridge / reserve on one relay failed (`CircuitClientCoordinator::Err`; docs/ERRORS.md). */
enum class CircuitTunnelErr : int32_t {
  Ok = 0,
  NotStarted,
  /** No bridge target given. */
  BadRequest,
  /** No address for the relay. */
  RelayNoEndpoint,
  /** Reaching the relay failed (the link's chain is in `message`). */
  RelayLinkFailed,
  /** The circuit channel to the relay did not open, or the request could not be sent. */
  ChannelFailed,
  /** The relay did not answer in time. */
  Timeout,
  /** The channel or the link to the relay went away before its answer. */
  Closed,
  Aborted,
  /** The relay's answer could not be read. */
  BadReply,
  /** The relay: the target is not parked on it. */
  TargetNotReserved,
  /** The relay: it could not reach the target. */
  TargetUnreachable,
  RelayBusy,
  /** The relay refused us (scope, not ready, a request it does not take). */
  RelayRefused,
  Generic,
};

} // namespace pp::mesh
