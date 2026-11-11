#include "mesh/testing/park_harness.h"

#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "common/metrics/MetricsRegistry.h"
#include "crypto/MlDsa.h"
#include "crypto/PeerId.h"
#include "mesh/l4/circuit/client/CircuitClientCoordinator.h"
#include "mesh/l4/circuit/serve/CircuitRelayServer.h"
#include "mesh/testing/delayed_datagram_io.h"
#include "mesh/testing/mesh_harness_support.h"

#include <algorithm>
#include <sodium.h>

namespace pp::mesh::test {
namespace {

constexpr int64_t kStartMs = 1'000'000;

/** TEST-NET-3: dialable by the mesh's rules (not RFC1918, which the dial book refuses for relays). */
pp::adp::IpEndpoint BenchAddress(const int index) {
  return pp::adp::IpEndpoint::V4(203, 0, 113, static_cast<uint8_t>(10 + index), 4001);
}

} // namespace

pp::Roe<std::unique_ptr<ParkHarness>> ParkHarness::Create() {
  if (sodium_init() < 0) {
    return pp::Error("park harness: sodium_init");
  }
  std::unique_ptr<ParkHarness> h(new ParkHarness());
  h->clock_ = std::make_shared<pp::adp::VirtualClock>(kStartMs);
  h->hub_ = pp::adp::MemoryDatagramIo::MakeHub();
  h->start_ms_ = kStartMs;
  std::string peer_id;
  std::string multiaddr;
  auto phone = h->MakeHost(BenchAddress(0), &peer_id, &multiaddr);
  if (!phone) {
    return phone.error();
  }
  h->phone_ = std::move(*phone);
  h->phone_peer_id_ = peer_id;
  ParkHarness* raw = h.get();
  CircuitRendezvousDeps deps;
  deps.mesh = [raw]() { return raw->phone_.get(); };
  deps.rendezvous_candidates = [raw]() { return raw->Candidates(/*seeds_only=*/false); };
  deps.bootstrap_seeds = [raw]() { return raw->Candidates(/*seeds_only=*/true); };
  deps.last_good_relay = []() { return std::string{}; };
  h->park_.SetDeps(std::move(deps));
  return h;
}

ParkHarness::~ParkHarness() {
  park_.Invalidate();
  if (caller_reach_) {
    caller_reach_->AbortPending();
    caller_reach_.reset(); // before the host whose circuit client it holds
  }
  if (caller_) {
    caller_->Stop();
  }
  if (phone_) {
    phone_->Stop();
  }
  for (auto& relay : relays_) {
    if (relay.host) {
      relay.host->Stop();
    }
  }
}

pp::Roe<std::unique_ptr<MeshHost>> ParkHarness::MakeHost(const pp::adp::IpEndpoint& addr, std::string* peer_id,
                                                         std::string* multiaddr, const int64_t delay_ms) {
  auto keys = pp::MlDsa::GenerateKeyPair();
  if (!keys) {
    return keys.error();
  }
  pp::amp::MshIdentity identity;
  identity.ml_dsa_secret_key = std::move(keys->secret_key);
  identity.ml_dsa_public_key = std::move(keys->public_key);
  auto id = pp::PeerIdFromMlDsaPublicKey(identity.ml_dsa_public_key);
  if (!id) {
    return id.error();
  }
  pp::amp::AmpStack::Config cfg;
  cfg.identity = std::move(identity);
  cfg.local_peer_id = *id;
  cfg.link_config = AmpMeshTestLinkConfig();
  std::shared_ptr<pp::adp::DatagramIo> io = std::make_shared<pp::adp::MemoryDatagramIo>(hub_, addr);
  if (delay_ms > 0) {
    io = std::make_shared<DelayedDatagramIo>(std::move(io), clock_, delay_ms);
  }
  auto stack = pp::amp::AmpStack::Create(io, clock_, cfg);
  if (!stack) {
    return stack.error();
  }
  (*stack)->GetEndpoint().SetAcceptEnabled(true); // a relay takes inbound associations (the phone's too: harmless)
  auto ma = pp::amp::FormatAdpMultiaddr(addr, *id);
  if (!ma) {
    return ma.error();
  }
  auto host = std::make_unique<MeshHost>(MeshExecutors{}, MetricsRegistry::Global()); // manual drive
  if (auto attached = host->AttachAmpStack(std::move(*stack), *ma); !attached) {
    return attached.error();
  }
  *peer_id = *id;
  *multiaddr = *ma;
  return host;
}

pp::Roe<std::string> ParkHarness::AddRelay(const RelayKind kind, const bool seed, const int64_t rtt_ms) {
  const int index = static_cast<int>(relays_.size()) + 1;
  Relay relay;
  relay.seed = seed;
  if (kind == RelayKind::Answers || kind == RelayKind::Slow) {
    auto host = MakeHost(BenchAddress(index), &relay.peer_id, &relay.multiaddr, kind == RelayKind::Slow ? rtt_ms : 0);
    if (!host) {
      return host.error();
    }
    relay.host = std::move(*host);
    if (auto* server = relay.host->AmpCircuitServer()) {
      server->SetServeInbound(true); // a relay node (AttachAmpStack hosts no relay services on its own)
    }
  } else {
    // An identity nobody holds, at an address nothing reads: the phone's handshakes go unanswered.
    auto keys = pp::MlDsa::GenerateKeyPair();
    if (!keys) {
      return keys.error();
    }
    auto id = pp::PeerIdFromMlDsaPublicKey(keys->public_key);
    if (!id) {
      return id.error();
    }
    auto ma = pp::amp::FormatAdpMultiaddr(BenchAddress(index), *id);
    if (!ma) {
      return ma.error();
    }
    relay.peer_id = *id;
    relay.multiaddr = *ma;
  }
  relays_.push_back(std::move(relay));
  return relays_.back().peer_id;
}

std::vector<MeshHopCandidate> ParkHarness::Candidates(const bool seeds_only) const {
  std::vector<MeshHopCandidate> out;
  for (const Relay& relay : relays_) {
    if (seeds_only && !relay.seed) {
      continue;
    }
    MeshHopCandidate hop;
    hop.peer_id = relay.peer_id;
    hop.multiaddr = relay.multiaddr;
    hop.advertises_circuit_relay = true;
    hop.roles = kReachRoleRendezvous | (relay.seed ? kReachRoleSeed : 0u);
    out.push_back(std::move(hop));
  }
  return out;
}

pp::Roe<void> ParkHarness::AddCaller() {
  std::string peer_id;
  std::string multiaddr;
  auto caller = MakeHost(BenchAddress(100), &peer_id, &multiaddr);
  if (!caller) {
    return caller.error();
  }
  caller_ = std::move(*caller);
  auto circuit = caller_->CircuitDeps();
  auto chat = caller_->ChatDeps();
  if (!circuit || !chat) {
    return pp::Error("park harness: the caller has no circuit client");
  }
  caller_reach_ = std::make_unique<AmpCircuitHopReach>(
      circuit->tunnel, circuit->hops, circuit->links, chat->io.io_pump,
      [this](const std::string& exclude) {
        std::vector<std::string> ids;
        for (const Relay& relay : relays_) {
          if (relay.peer_id != exclude) {
            (void)caller_->ChatDeps()->links.RegisterEndpoint(relay.peer_id, relay.multiaddr);
            ids.push_back(relay.peer_id);
          }
        }
        return ids;
      },
      AmpCircuitHopReach::TryPunchAsync{}, AmpCircuitHopReach::TryPunchViaIntroducerAsync{}, chat->io.post_io,
      chat->io.post_after);
  caller_reach_->SetClock([clock = clock_]() { return clock->NowMs(); });
  return {};
}

MeshHost* ParkHarness::RelayHost(const std::string& peer_id) {
  for (auto& relay : relays_) {
    if (relay.peer_id == peer_id) {
      return relay.host.get();
    }
  }
  return nullptr;
}

int64_t ParkHarness::ElapsedMs() const {
  return clock_->NowMs() - start_ms_;
}

void ParkHarness::TickAll() {
  phone_->Tick();
  if (caller_) {
    caller_->Tick();
  }
  for (auto& relay : relays_) {
    if (relay.host) {
      relay.host->Tick();
    }
  }
}

bool ParkHarness::RunUntil(const std::function<bool()>& done, const int64_t max_ms, const int64_t step_ms) {
  const int64_t until = ElapsedMs() + max_ms;
  TickAll();
  while (!done()) {
    if (ElapsedMs() >= until) {
      return false;
    }
    clock_->Advance(step_ms);
    TickAll();
  }
  return true;
}

std::vector<std::string> ParkHarness::Parked() {
  auto* client = phone_->AmpCircuitClient();
  return client ? client->ParkedRelays() : std::vector<std::string>{};
}

bool ParkHarness::ParkedOn(const std::string& relay_peer_id) {
  const auto parked = Parked();
  return std::find(parked.begin(), parked.end(), relay_peer_id) != parked.end();
}

} // namespace pp::mesh::test
