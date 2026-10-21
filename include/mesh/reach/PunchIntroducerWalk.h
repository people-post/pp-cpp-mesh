#pragma once

#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/Module.h"
#include "mesh/reach/PeerReachRecord.h"
#include "mesh/reachability/punch/PunchLinkOps.h"
#include "mesh/host/MeshHost.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace pp::mesh {

/** Cold-punch introducers in preference order: contacts first, then configured seeds. */
struct MeshPunchIntroducers {
  std::vector<std::string> contact_peer_ids;
  std::vector<std::string> seed_peer_ids;
};

struct PunchIntroducerDeps {
  std::function<MeshHost*()> mesh;
  /** Product policy: who may introduce us (contacts, seeds) — `domain/mesh` does not read contacts. */
  std::function<MeshPunchIntroducers()> introducers;
  /** What the reach record says about a peer on the current network (none: every peer Unknown). */
  std::function<PeerReachFacts(const std::string& peer_id)> reach_facts;
};

/**
 * The punch step of reach (`AmpCircuitHopReach` calls it when dial and circuit are not enough):
 * a cold punch walks introducers in order, moving on when one does not know the target (B29), and
 * falls back to a consumer's own signaling once they are exhausted (H012); an upgrade punch goes
 * through a given introducer (circuit → direct). Mechanism is `AmpPunchCoordinator`.
 *
 * The walk skips an introducer the reach record holds down (unless its link is up) and dials at
 * most kReachMaxColdDialsPerRound introducers that are not connected (docs/PEER_REACH.md § 5).
 *
 * Threading: called from reach on the Amp IO strand; completions from the punch
 * coordinator. Deps and the signaling hook are set by the owner before traffic.
 */
class PunchIntroducerWalk : public Module {
public:
  /** Why a punch did not land (docs/ERRORS.md). */
  enum class Err : int32_t {
    Ok = 0,
    /** Punch is not running. */
    Unavailable,
    NoIntroducer,
    /** Every introducer was asked; none could get to the target. */
    IntroducersExhausted,
    /** The target declines us. */
    Declined,
    /** The punch ran and no path won. */
    NoPath,
    /** Otherwise (protocol, send, timeout, the signaling fallback). */
    Failed,
    Generic,
  };
  using Failure = CodedFailure<Err>;
  using WalkRoe = CodedRoe<void, Err>;
  using Done = std::function<void(WalkRoe)>;

  /** One punch's failure, as the walk's (its chain in `message`). */
  static Failure WrapPunchFailure(const PunchFailure& punch);
  /** The punch failed in a way another introducer may not: it cannot get to the target. */
  static bool AnotherIntroducerMayHelp(PunchErr code);

  using SignalingPunchFn = std::function<void(const std::string& target_peer_id,
                                              const std::vector<std::string>& my_addrs,
                                              std::function<void(Roe<void>)> on_done)>;

  PunchIntroducerWalk();
  PunchIntroducerWalk(const PunchIntroducerWalk&) = delete;
  PunchIntroducerWalk& operator=(const PunchIntroducerWalk&) = delete;

  void SetDeps(PunchIntroducerDeps deps);
  /** Last-resort punch through a consumer's own signaling (calls: call-control). */
  void SetSignalingPunch(SignalingPunchFn punch);

  void TryColdPunchAsync(const std::string& target_peer_id, Done on_done);
  void TryUpgradePunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id, Done on_done);

private:
  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }

  PunchIntroducerDeps deps_;
  SignalingPunchFn signaling_punch_;
};

} // namespace pp::mesh
