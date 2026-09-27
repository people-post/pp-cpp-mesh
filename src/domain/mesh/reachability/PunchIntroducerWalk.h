#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "domain/mesh/host/MeshHost.h"

#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Cold-punch introducers in preference order: contacts first, then configured seeds. */
struct MeshPunchIntroducers {
  std::vector<std::string> contact_peer_ids;
  std::vector<std::string> seed_peer_ids;
};

struct PunchIntroducerDeps {
  std::function<MeshHost*()> mesh;
  /** Product policy: who may introduce us (contacts, seeds) — `domain/mesh` does not read contacts. */
  std::function<MeshPunchIntroducers()> introducers;
};

/**
 * The punch step of reach (`AmpCircuitHopReach` calls it when dial and circuit are not enough):
 * a cold punch walks introducers in order, moving on when one does not know the target (B29), and
 * falls back to a consumer's own signaling once they are exhausted (H012); an upgrade punch goes
 * through a given introducer (circuit → direct). Mechanism is `AmpPunchCoordinator`.
 *
 * Threading: called from reach on the Amp IO / MeshControl threads; completions from the punch
 * coordinator. Deps and the signaling hook are set by the owner before traffic.
 */
class PunchIntroducerWalk : public Module {
public:
  using SignalingPunchFn = std::function<void(const std::string& target_peer_id,
                                              const std::vector<std::string>& my_addrs,
                                              std::function<void(Roe<void>)> on_done)>;

  PunchIntroducerWalk();
  PunchIntroducerWalk(const PunchIntroducerWalk&) = delete;
  PunchIntroducerWalk& operator=(const PunchIntroducerWalk&) = delete;

  void SetDeps(PunchIntroducerDeps deps);
  /** Last-resort punch through a consumer's own signaling (calls: call-control). */
  void SetSignalingPunch(SignalingPunchFn punch);

  void TryColdPunchAsync(const std::string& target_peer_id, std::function<void(Roe<void>)> on_done);
  void TryUpgradePunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                            std::function<void(Roe<void>)> on_done);

private:
  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }

  PunchIntroducerDeps deps_;
  SignalingPunchFn signaling_punch_;
};

} // namespace pbr
