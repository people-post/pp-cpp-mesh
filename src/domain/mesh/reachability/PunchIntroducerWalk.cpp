#include "domain/mesh/reachability/PunchIntroducerWalk.h"

#include "domain/mesh/reachability/AmpPunchCoordinator.h"
#include "domain/mesh/reachability/PunchLogic.h"

#include <memory>
#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

void CompletePunch(const std::function<void(Roe<void>)>& on_done, AmpPunchCoordinator::PunchRoe punched,
                   const char* fail_fallback) {
  if (!punched) {
    on_done(Error(punched.error().message));
  } else if (!punched->ok) {
    on_done(Error(punched->error.empty() ? fail_fallback : punched->error));
  } else {
    on_done(Roe<void>());
  }
}

} // namespace

PunchIntroducerWalk::PunchIntroducerWalk() {
  redirectLogger("PunchIntroducerWalk");
}

void PunchIntroducerWalk::SetDeps(PunchIntroducerDeps deps) {
  deps_ = std::move(deps);
}

void PunchIntroducerWalk::SetSignalingPunch(SignalingPunchFn punch) {
  signaling_punch_ = std::move(punch);
}

void PunchIntroducerWalk::TryColdPunchAsync(const std::string& target_peer_id, std::function<void(Roe<void>)> on_done) {
  MeshHost* m = mesh();
  auto circuit = m ? m->CircuitDeps() : std::nullopt;
  auto* punch = m ? m->AmpPunch() : nullptr;
  if (!circuit || !punch || !punch->IsStarted()) {
    on_done(Error("amp punch unavailable"));
    return;
  }
  IChatPeerLinks* links = &circuit->links;
  MeshPunchIntroducers introducers = deps_.introducers ? deps_.introducers() : MeshPunchIntroducers{};
  auto has_ep = [links](const std::string& id) { return links->GetLinkSnapshot(id).has_endpoint; };
  auto is_conn = [links](const std::string& id) { return links->IsConnected(id); };

  // B29: if the first introducer misses (target unknown / channel fail), try the next. The attempt
  // holds its own retry function; the chain ends when it settles (no self-owning cycle past that).
  struct IntroAttempt {
    std::unordered_set<std::string> tried;
    std::function<void()> try_next;
  };
  auto attempt = std::make_shared<IntroAttempt>();
  std::weak_ptr<IntroAttempt> weak = attempt;
  attempt->try_next = [this, weak, punch, target_peer_id, introducers = std::move(introducers), has_ep, is_conn,
                       on_done = std::move(on_done)]() {
    auto self = weak.lock();
    if (!self) {
      return;
    }
    auto intro = PickPunchIntroducer(introducers.contact_peer_ids, introducers.seed_peer_ids, target_peer_id, has_ep,
                                     is_conn, self->tried);
    if (!intro) {
      if (signaling_punch_) {
        log().info << "punch introducers exhausted — H012 signaling fallback target=" << target_peer_id;
        signaling_punch_(target_peer_id, punch->LocalCandidateAddrs(), on_done);
        return;
      }
      on_done(Error(self->tried.empty() ? "no punch introducer" : "punch introducers exhausted"));
      return;
    }
    self->tried.insert(*intro);
    punch->TryColdPunchAsync(
        *intro, target_peer_id, punch->LocalCandidateAddrs(),
        [self, on_done](AmpPunchCoordinator::PunchRoe punched) {
          if (punched && punched->ok) {
            CompletePunch(on_done, std::move(punched), "punch failed");
            return;
          }
          const std::string err =
              !punched ? punched.error().message : (punched->error.empty() ? std::string("punch failed") : punched->error);
          const bool try_another = err.find("introducer") != std::string::npos ||
                                   err.find("not registered") != std::string::npos;
          if (try_another) {
            self->try_next();
            return;
          }
          on_done(Error(err));
        },
        2000);
  };
  attempt->try_next();
}

void PunchIntroducerWalk::TryUpgradePunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                                          std::function<void(Roe<void>)> on_done) {
  MeshHost* m = mesh();
  auto* punch = m ? m->AmpPunch() : nullptr;
  if (!punch || !punch->IsStarted()) {
    on_done(Error("amp punch unavailable"));
    return;
  }
  punch->TryUpgradePunchAsync(
      introducer_peer_key, target_peer_id, punch->LocalCandidateAddrs(),
      [on_done = std::move(on_done)](AmpPunchCoordinator::PunchRoe punched) {
        CompletePunch(on_done, std::move(punched), "upgrade punch failed");
      },
      2000);
}

} // namespace pbr
