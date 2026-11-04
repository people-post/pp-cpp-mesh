#pragma once

#include "amp/L1/Clock.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "common/Error.h"
#include "mesh/host/MeshHost.h"
#include "mesh/reach/CircuitRendezvousCoordinator.h"
#include "mesh/types/MeshHopTypes.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pp::mesh::test {

/**
 * The park on a bench (docs/PEER_REACH.md § the park): a phone's MeshHost and its CircuitRendezvousCoordinator,
 * and relays, on one MemoryDatagramHub and one VirtualClock, driven by hand (no threads). The coordinator is the
 * product's; only its deps (the surface, the seeds) are the bench's.
 *
 * A relay either answers (a MeshHost whose circuit relay server takes reservations) or is unreachable (an address
 * nothing reads: every datagram to it is lost, as to a relay behind a firewall that drops them). Time moves only in
 * RunUntil, so a test can say how long the park took, in virtual milliseconds.
 */
class ParkHarness {
public:
  enum class RelayKind {
    Answers,
    Unreachable,
  };

  static pp::Roe<std::unique_ptr<ParkHarness>> Create();
  ~ParkHarness();
  ParkHarness(const ParkHarness&) = delete;
  ParkHarness& operator=(const ParkHarness&) = delete;

  /**
   * A relay on the phone's rendezvous surface (in the order added), and one of its bootstrap seeds when `seed`.
   * Returns its PeerId.
   */
  pp::Roe<std::string> AddRelay(RelayKind kind, bool seed = true);

  CircuitRendezvousCoordinator& Park() { return park_; }
  MeshHost& Phone() { return *phone_; }
  /** An answering relay's host (null for an unreachable one, or an unknown PeerId). */
  MeshHost* RelayHost(const std::string& peer_id);

  /** Virtual milliseconds since Create. */
  int64_t ElapsedMs() const;
  /** Moves virtual time on in `step_ms` steps, ticking every host, until `done()` or `max_ms` have passed. */
  bool RunUntil(const std::function<bool()>& done, int64_t max_ms, int64_t step_ms = 10);
  /** The relays the phone holds a circuit reservation on. */
  std::vector<std::string> Parked();
  bool ParkedOn(const std::string& relay_peer_id);

private:
  ParkHarness() = default;
  struct Relay {
    std::string peer_id;
    std::string multiaddr;
    bool seed = false;
    std::unique_ptr<MeshHost> host;  // null: unreachable
  };
  pp::Roe<std::unique_ptr<MeshHost>> MakeHost(const pp::adp::IpEndpoint& addr, std::string* peer_id,
                                              std::string* multiaddr);
  std::vector<MeshHopCandidate> Candidates(bool seeds_only) const;
  void TickAll();

  std::shared_ptr<pp::adp::VirtualClock> clock_;
  std::shared_ptr<pp::adp::MemoryDatagramHub> hub_;
  int64_t start_ms_ = 0;
  std::unique_ptr<MeshHost> phone_;
  std::vector<Relay> relays_;
  CircuitRendezvousCoordinator park_;
};

} // namespace pp::mesh::test
