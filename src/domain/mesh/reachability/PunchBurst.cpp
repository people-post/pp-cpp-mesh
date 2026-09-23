#include "domain/mesh/reachability/PunchBurst.h"

#include "amp/link/AdpMultiaddr.h"
#include "amp/link/PeerLink.h"
#include "common/SettledWait.h"
#include "domain/mesh/reachability/AmpPunchCoordinator.h"
#include "domain/mesh/reachability/PunchLogic.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

} // namespace

bool PeerAlreadyConnectedDirect(pp::amp::PeerLinkManager& links, const std::string& peer_id) {
  if (peer_id.empty()) {
    return false;
  }
  // Nested/circuit carrier links must not short-circuit punch (L3.25c upgrade-from-circuit).
  if (auto* link = links.FindLinkByPeerId(peer_id)) {
    return link->Phase() == pp::amp::PeerLinkPhase::Connected && !link->IsCarrierBacked();
  }
  return false;
}

PunchBurstResult BurstDialCandidates(pp::amp::PeerLinkManager& links, std::function<void()> io_pump,
                                     const std::vector<std::string>& targets, int window_ms) {
  PunchBurstResult out;
  const auto addrs = SanitizePunchAddrs(targets);
  if (addrs.empty()) {
    out.error = "no peer_addrs";
    return out;
  }
  const int window = window_ms > 0 ? window_ms : 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(window);

  auto abort_burst_key = [&](const std::string& key) {
    links.AbortInflightDial(key);
    if (io_pump) {
      io_pump();
      io_pump();
    }
  };

  for (size_t i = 0; i < addrs.size(); ++i) {
    if (Clock::now() >= deadline) {
      break;
    }
    const std::string& ma = addrs[i];
    auto parsed = pp::amp::ParseAdpMultiaddr(ma);
    if (!parsed) {
      out.error = "peer addr is not an ADP multiaddr";
      out.dialed = ma;
      continue;
    }
    const std::string peer_id = parsed->peer_id;

    if (PeerAlreadyConnectedDirect(links, peer_id)) {
      out.ok = true;
      out.dialed = ma;
      out.error.clear();
      return out;
    }

    const std::string key = "punch:burst:" + std::to_string(i) + ":" +
                            peer_id.substr(0, std::min<size_t>(peer_id.size(), 12));

    if (auto registered = links.RegisterEndpoint(key, ma); !registered) {
      if (PeerAlreadyConnectedDirect(links, peer_id)) {
        out.ok = true;
        out.dialed = ma;
        out.error.clear();
        return out;
      }
      out.error = registered.error().message;
      out.dialed = ma;
      continue;
    }

    if (io_pump) {
      io_pump();
      io_pump();
    }

    SettledWait<void> wait;
    links.EnsureAssociation(key, [wait](pp::amp::PeerLinkManager::LinkRoe result) {
      if (result) {
        wait.Finish(Roe<void>());
      } else {
        wait.Finish(Roe<void>(Error(AmpPunchCoordinator::WrapLinkFailure(result.error()).message)));
      }
    });

    while (Clock::now() < deadline && !wait.IsSettled()) {
      if (PeerAlreadyConnectedDirect(links, peer_id)) {
        out.ok = true;
        out.dialed = ma;
        out.error.clear();
        return out;
      }
      if (io_pump) {
        io_pump();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    auto dialed = wait.Wait(std::chrono::milliseconds(1), Error("punch burst dial timed out"));
    if (PeerAlreadyConnectedDirect(links, peer_id)) {
      out.ok = true;
      out.dialed = ma;
      out.error.clear();
      return out;
    }
    if (dialed) {
      if (auto* link = links.FindLink(key)) {
        if (link->Phase() == pp::amp::PeerLinkPhase::Connected && !link->IsCarrierBacked()) {
          out.ok = true;
          out.dialed = ma;
          out.error.clear();
          return out;
        }
      }
      abort_burst_key(key);
      out.error = "punch burst associated without a direct PeerLink";
      out.dialed = ma;
      continue;
    }

    const std::string err = dialed.error().message;
    if (err.find("already open") != std::string::npos) {
      const auto race_deadline = std::min(deadline, Clock::now() + std::chrono::milliseconds(500));
      while (Clock::now() < race_deadline && !PeerAlreadyConnectedDirect(links, peer_id)) {
        if (io_pump) {
          io_pump();
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      if (PeerAlreadyConnectedDirect(links, peer_id)) {
        out.ok = true;
        out.dialed = ma;
        out.error.clear();
        return out;
      }
    }
    abort_burst_key(key);
    out.error = err;
    out.dialed = ma;
  }
  if (!out.ok && out.error.empty()) {
    out.error = "punch burst window expired";
  }
  return out;
}

void BurstDialCandidatesAsync(pp::amp::PeerLinkManager& links,
                              std::function<void(std::function<void()>)> post_io,
                              const std::vector<std::string>& targets, int window_ms,
                              std::function<void(PunchBurstResult)> on_done,
                              std::function<void(std::function<void()>)> settle_on) {
  if (!post_io) {
    if (!on_done) {
      return;
    }
    // No PostToIo: sync dial; still honor settle_on for Abort/complete stacking.
    if (settle_on) {
      settle_on([links_ptr = &links, targets, window_ms, on_done = std::move(on_done)]() mutable {
        on_done(BurstDialCandidates(*links_ptr, {}, targets, window_ms));
      });
    } else {
      on_done(BurstDialCandidates(links, {}, targets, window_ms));
    }
    return;
  }
  const auto addrs = SanitizePunchAddrs(targets);
  if (addrs.empty()) {
    PunchBurstResult early;
    early.error = "no peer_addrs";
    if (!on_done) {
      return;
    }
    if (settle_on) {
      settle_on([on_done = std::move(on_done), early = std::move(early)]() mutable {
        on_done(std::move(early));
      });
    } else {
      on_done(std::move(early));
    }
    return;
  }

  pp::amp::PeerLinkManager* links_ptr = &links;
  struct State {
    std::atomic<bool> settled{false};
    Clock::time_point deadline{};
    std::vector<std::string> keys;
    std::vector<std::string> peer_ids;
    std::vector<std::string> multiaddrs;
    std::string last_error;
    std::function<void(PunchBurstResult)> on_done;
    std::function<void(std::function<void()>)> settle_on;
  };
  auto state = std::make_shared<State>();
  state->deadline = Clock::now() + std::chrono::milliseconds(window_ms > 0 ? window_ms : 2000);
  state->on_done = std::move(on_done);
  state->settle_on = std::move(settle_on);

  auto finish = [state, links_ptr](PunchBurstResult result) {
    if (state->settled.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    // Mark settled on the PostToIo poll stack, but Abort + on_done must leave DrainPostedIo
    // (nested Tick / session teardown SEHs on Windows). Prefer SchedulePark when available.
    auto deliver = [state, links_ptr, result = std::move(result)]() mutable {
      for (const std::string& key : state->keys) {
        auto* link = links_ptr->FindLink(key);
        if (!result.ok || !link || link->Phase() != pp::amp::PeerLinkPhase::Connected) {
          links_ptr->AbortInflightDial(key);
        }
      }
      if (state->on_done) {
        state->on_done(std::move(result));
      }
    };
    if (state->settle_on) {
      state->settle_on(std::move(deliver));
    } else {
      deliver();
    }
  };

  for (size_t i = 0; i < addrs.size(); ++i) {
    const std::string& ma = addrs[i];
    auto parsed = pp::amp::ParseAdpMultiaddr(ma);
    if (!parsed) {
      state->last_error = "peer addr is not an ADP multiaddr";
      continue;
    }
    const std::string peer_id = parsed->peer_id;
    if (PeerAlreadyConnectedDirect(*links_ptr, peer_id)) {
      PunchBurstResult ok;
      ok.ok = true;
      ok.dialed = ma;
      finish(std::move(ok));
      return;
    }
    const std::string key = "punch:burst:" + std::to_string(i) + ":" +
                            peer_id.substr(0, std::min<size_t>(peer_id.size(), 12));
    if (auto registered = links_ptr->RegisterEndpoint(key, ma); !registered) {
      if (PeerAlreadyConnectedDirect(*links_ptr, peer_id)) {
        PunchBurstResult ok;
        ok.ok = true;
        ok.dialed = ma;
        finish(std::move(ok));
        return;
      }
      state->last_error = registered.error().message;
      continue;
    }
    state->keys.push_back(key);
    state->peer_ids.push_back(peer_id);
    state->multiaddrs.push_back(ma);
    links_ptr->EnsureAssociation(key, [state](pp::amp::PeerLinkManager::LinkRoe result) {
      if (state->settled.load(std::memory_order_acquire)) {
        return;
      }
      if (!result) {
        state->last_error = AmpPunchCoordinator::WrapLinkFailure(result.error()).message;
      }
      // Wins are observed on the PostToIo poll (PeerId adopt / non-carrier Connected).
    });
  }

  if (state->keys.empty()) {
    PunchBurstResult fail;
    fail.error = state->last_error.empty() ? "no peer_addrs" : state->last_error;
    finish(std::move(fail));
    return;
  }

  auto poll = std::make_shared<std::function<void()>>();
  *poll = [state, post_io, finish, poll, links_ptr]() {
    if (state->settled.load(std::memory_order_acquire)) {
      return;
    }
    for (size_t i = 0; i < state->peer_ids.size(); ++i) {
      // Require PeerId-visible non-carrier win so callers see FindLinkByPeerId after settle.
      if (PeerAlreadyConnectedDirect(*links_ptr, state->peer_ids[i])) {
        PunchBurstResult ok;
        ok.ok = true;
        ok.dialed = state->multiaddrs[i];
        finish(std::move(ok));
        return;
      }
    }
    if (Clock::now() >= state->deadline) {
      PunchBurstResult fail;
      fail.error = state->last_error.empty() ? "punch burst window expired" : state->last_error;
      if (!state->multiaddrs.empty()) {
        fail.dialed = state->multiaddrs.back();
      }
      finish(std::move(fail));
      return;
    }
    post_io([poll, state]() {
      if (!state->settled.load(std::memory_order_acquire)) {
        (*poll)();
      }
    });
  };
  post_io([poll]() { (*poll)(); });
}

} // namespace pbr
