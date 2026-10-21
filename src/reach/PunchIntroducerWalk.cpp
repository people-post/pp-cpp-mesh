#include "mesh/reach/PunchIntroducerWalk.h"

#include "mesh/reachability/punch/AmpPunchCoordinator.h"
#include "mesh/reachability/punch/PunchLogic.h"

#include <memory>
#include <unordered_set>
namespace pp::mesh {
namespace {

using Err = PunchIntroducerWalk::Err;
using Failure = PunchIntroducerWalk::Failure;

void CompletePunch(const PunchIntroducerWalk::Done& on_done, AmpPunchCoordinator::PunchRoe punched) {
  if (!punched) {
    on_done(PunchIntroducerWalk::WrapPunchFailure(punched.error()));
  } else if (!punched->ok) {
    on_done(Failure::Of(Err::NoPath, punched->error.empty() ? "punch failed" : punched->error));
  } else {
    on_done(PunchIntroducerWalk::WalkRoe());
  }
}

} // namespace

PunchIntroducerWalk::Failure PunchIntroducerWalk::WrapPunchFailure(const PunchFailure& punch) {
  Err code = Err::Failed;
  switch (punch.GetCode()) {
  case PunchErr::NotStarted:
    code = Err::Unavailable;
    break;
  case PunchErr::Declined:
    code = Err::Declined;
    break;
  case PunchErr::PunchFailed:
    code = Err::NoPath;
    break;
  default:
    code = Err::Failed;
    break;
  }
  return Failure::Of(code, detail::AppendFrom("punch walk: punch failed", "punch", punch.message));
}

bool PunchIntroducerWalk::AnotherIntroducerMayHelp(const PunchErr code) {
  return code == PunchErr::TargetUnknown || code == PunchErr::EndpointNotRegistered || code == PunchErr::LinkFailed ||
         code == PunchErr::ChannelFailed;
}

PunchIntroducerWalk::PunchIntroducerWalk() {
  redirectLogger("PunchIntroducerWalk");
}

void PunchIntroducerWalk::SetDeps(PunchIntroducerDeps deps) {
  deps_ = std::move(deps);
}

void PunchIntroducerWalk::SetSignalingPunch(SignalingPunchFn punch) {
  signaling_punch_ = std::move(punch);
}

void PunchIntroducerWalk::TryColdPunchAsync(const std::string& target_peer_id, Done on_done) {
  MeshHost* m = mesh();
  auto circuit = m ? m->CircuitDeps() : std::nullopt;
  auto* punch = m ? m->AmpPunch() : nullptr;
  if (!circuit || !punch || !punch->IsStarted()) {
    on_done(Failure::Of(Err::Unavailable, "amp punch unavailable"));
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
        signaling_punch_(target_peer_id, punch->LocalCandidateAddrs(), [on_done](Roe<void> signaled) {
          on_done(signaled ? WalkRoe()
                           : WalkRoe(Failure::Of(Err::Failed, detail::AppendFrom("punch walk: signaling punch failed",
                                                                                  "signaling", signaled.error().message))));
        });
        return;
      }
      on_done(self->tried.empty() ? Failure::Of(Err::NoIntroducer, "no punch introducer")
                                  : Failure::Of(Err::IntroducersExhausted, "punch introducers exhausted"));
      return;
    }
    self->tried.insert(*intro);
    punch->TryColdPunchAsync(
        *intro, target_peer_id, punch->LocalCandidateAddrs(),
        [self, on_done](AmpPunchCoordinator::PunchRoe punched) {
          if (!punched && AnotherIntroducerMayHelp(punched.error().GetCode())) {
            self->try_next();
            return;
          }
          CompletePunch(on_done, std::move(punched));
        },
        2000);
  };
  attempt->try_next();
}

void PunchIntroducerWalk::TryUpgradePunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                                               Done on_done) {
  MeshHost* m = mesh();
  auto* punch = m ? m->AmpPunch() : nullptr;
  if (!punch || !punch->IsStarted()) {
    on_done(Failure::Of(Err::Unavailable, "amp punch unavailable"));
    return;
  }
  punch->TryUpgradePunchAsync(
      introducer_peer_key, target_peer_id, punch->LocalCandidateAddrs(),
      [on_done = std::move(on_done)](AmpPunchCoordinator::PunchRoe punched) { CompletePunch(on_done, std::move(punched)); },
      2000);
}

} // namespace pp::mesh
