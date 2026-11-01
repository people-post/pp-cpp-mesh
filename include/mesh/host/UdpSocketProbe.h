#pragma once

#include "amp/L1/Types.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace pp::mesh {

/**
 * Is Amp's UDP socket still alive? (#416) iOS can leave an app's socket dead across a suspend or a
 * path change: `sendto` fails or nothing is ever received, while HTTPS keeps working — and Amp
 * never says so (receive errors end the pump quietly). The probe sends a short datagram from the
 * socket to its own port and waits for any datagram to arrive; a short one is rejected by Amp
 * before any association sees it. A dead socket is reopened on its port, so peers and relays still
 * reach us where they did.
 */
struct UdpSocketProbeResult {
  bool ok = false;
  /** Why not: the send's error (with errno) or that nothing arrived. */
  std::string error;
};

/** Why the socket was checked. */
enum class UdpSocketCheckReason : uint8_t {
  /** An online network change (MeshHost::OnLocalNetworkChanged). */
  NetworkChange,
  /** The product asked (the app came back to the foreground). */
  Requested,
};

/** What `MeshHost::EnsureUdpSocketAlive` found and did. */
struct UdpSocketCheck {
  UdpSocketCheckReason reason = UdpSocketCheckReason::Requested;
  /** The socket answered the first probe: nothing done. */
  bool was_alive = false;
  /** It did not: it was closed and bound again on its port (`DatagramIo::Reopen`). */
  bool reopened = false;
  /** Alive now: the first probe, or the one after the reopen, answered. */
  bool alive = false;
  /** Why it was dead, and why the reopen or the probe after it failed. */
  std::string error;
};

/** One wait per send: a busy lane gets a second, longer chance before the socket is called dead. */
inline constexpr std::chrono::milliseconds kUdpSocketProbeFirstWait{300};
inline constexpr std::chrono::milliseconds kUdpSocketProbeSecondWait{1000};

/** Where the probe goes: the socket's own address, or loopback of its family when it is bound to any. */
adp::IpEndpoint UdpSocketProbeTarget(const adp::IpEndpoint& local);

} // namespace pp::mesh
