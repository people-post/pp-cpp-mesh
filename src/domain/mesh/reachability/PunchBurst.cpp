#include "domain/mesh/reachability/PunchBurst.h"

#include "amp/link/AdpMultiaddr.h"
#include "amp/link/PeerLink.h"
#include "common/SettledWait.h"
#include "domain/mesh/reachability/AmpPunchCoordinator.h"
#include "domain/mesh/reachability/PunchLogic.h"

#include <chrono>
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
  return links.IsConnectedToPeerId(peer_id);
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

} // namespace pbr
