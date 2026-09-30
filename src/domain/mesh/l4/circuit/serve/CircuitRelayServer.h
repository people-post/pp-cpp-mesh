#pragma once

#include "domain/mesh/l4/shared/RelayRuntimeStats.h"
#include "amp/link/MeshRuntime.h"
#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"

#include "common/PbrCompat.h"

#include <cstddef>
#include <memory>

namespace pbr {

/**
 * Serving side of `/pp-browser/circuit/1.0.0` on MeshRuntime ([A022]): a relay that answers
 * `op=bridge` (dial the target, then splice the two legs) and `op=reserve` (park an answerer's
 * channel so a later bridge to its PeerId needs no dial into its NAT). Admission (scope, contacts,
 * K003 standby limits) is decided here. MeshHost owns one when Amp is up; the protocol handler is
 * always registered and `SetServeInbound(false)` refuses new work.
 */
class CircuitRelayServer {
public:
  explicit CircuitRelayServer(pp::amp::MeshRuntime& runtime);
  ~CircuitRelayServer();

  CircuitRelayServer(const CircuitRelayServer&) = delete;
  CircuitRelayServer& operator=(const CircuitRelayServer&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const;

  void SetAdmissionPolicy(CircuitRelayAdmissionPolicy policy);
  /** When false, inbound bridges / reserves are refused. */
  void SetServeInbound(bool serve);
  bool ServeInbound() const;
  /** K003 standby circuits served at once: relay-wide and per dialer PeerId (0 keeps the default). */
  void SetStandbyLimits(size_t max_standby, size_t max_per_dialer);
  /** Tunnels bridged / still in setup, and parked reservations (aggregates only). Any thread. */
  CircuitRelayRuntimeStats RuntimeStats() const;

  /** Close every served tunnel and parked reservation. */
  void AbortInflight();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
};

} // namespace pbr
