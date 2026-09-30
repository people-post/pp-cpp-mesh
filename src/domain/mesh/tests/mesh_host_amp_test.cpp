#include "amp/L1/Clock.h"
#include "amp/L1/LossyDatagramIo.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/link/LinkEvents.h"
#include "foundation/crypto/MlDsa.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "domain/mesh/tests/support/mesh_harness_support.h"
#include "domain/mesh/l4/media_relay/client/MediaRelayClientCoordinator.h"
#include "domain/mesh/l4/media_relay/serve/MediaRelayServer.h"
#include "domain/mesh/l4/circuit/client/CircuitClientCoordinator.h"
#include "domain/mesh/l4/circuit/serve/CircuitRelayServer.h"
#include "domain/mesh/host/LocalNetworkChange.h"
#include "domain/mesh/connectivity/MeshConnectivity.h"
#include "domain/mesh/host/MeshHost.h"
#include "common/metrics/MetricsRegistry.h"
#include "domain/mesh/media_plane/MeshMediaRelay.h"
#include "foundation/identity/PeerIdUtil.h"

#include <gtest/gtest.h>
#include <sodium.h>

namespace pbr {
namespace {

std::unique_ptr<pp::amp::AmpStack> MakeTestAmpStack(const std::shared_ptr<pp::adp::Clock>& clock,
                                                const std::shared_ptr<pp::adp::DatagramIo>& io,
                                                std::string* peer_id_out) {
  auto keys = MlDsa::GenerateKeyPair();
  if (!keys) {
    return nullptr;
  }
  pp::amp::MshIdentity identity;
  identity.ml_dsa_secret_key = std::move(keys->secret_key);
  identity.ml_dsa_public_key = std::move(keys->public_key);
  auto peer_id = PeerIdFromMlDsaPublicKey(identity.ml_dsa_public_key);
  if (!peer_id) {
    return nullptr;
  }
  *peer_id_out = *peer_id;

  pp::amp::AmpStack::Config cfg;
  cfg.identity = std::move(identity);
  cfg.local_peer_id = *peer_id;
  cfg.link_config = pbr::test::AmpMeshTestLinkConfig();

  auto stack = pp::amp::AmpStack::Create(io, clock, cfg);
  if (!stack) {
    return nullptr;
  }
  return std::move(*stack);
}

TEST(MeshHostAmpTest, AttachAmpStackParallelNoMeshHost) {
  ASSERT_GE(sodium_init(), 0);

  auto clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  const auto addr = pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
  auto io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr);

  std::string peer_id;
  auto stack = MakeTestAmpStack(clock, io, &peer_id);
  ASSERT_NE(stack, nullptr);
  auto ma = pp::amp::FormatAdpMultiaddr(addr, peer_id);
  ASSERT_TRUE(static_cast<bool>(ma));

  MeshHost host;
  ASSERT_TRUE(static_cast<bool>(host.AttachAmpStack(std::move(stack), *ma)));
  ASSERT_NE(host.Amp(), nullptr);
  EXPECT_TRUE(host.Amp()->IsStarted());
  EXPECT_EQ(host.AmpListenMultiaddr(), *ma);
  EXPECT_EQ(host.Amp()->Links().LocalCapability().listen_multiaddrs, std::vector<std::string>{*ma});
  ASSERT_NE(host.AmpCircuitClient(), nullptr);
  ASSERT_NE(host.AmpCircuitServer(), nullptr);
  ASSERT_NE(host.AmpMediaRelayServer(), nullptr);
  ASSERT_NE(host.AmpMediaRelayClientCoord(), nullptr);
  EXPECT_TRUE(host.AmpCircuitClient()->IsStarted());
  EXPECT_TRUE(host.AmpCircuitServer()->IsStarted());
  EXPECT_TRUE(host.AmpMediaRelayServer()->IsStarted());
  EXPECT_TRUE(host.AmpMediaRelayClientCoord()->IsStarted());
  ASSERT_NE(host.AmpCircuitHops(), nullptr);

  host.Tick();
  host.Stop();
  EXPECT_EQ(host.Amp(), nullptr);
  EXPECT_EQ(host.AmpCircuitClient(), nullptr);
  EXPECT_EQ(host.AmpCircuitServer(), nullptr);
  EXPECT_EQ(host.AmpMediaRelayServer(), nullptr);
  EXPECT_EQ(host.AmpMediaRelayClientCoord(), nullptr);
  EXPECT_EQ(host.AmpCircuitHops(), nullptr);
  EXPECT_TRUE(host.AmpListenMultiaddr().empty());
}

TEST(MeshHostAmpTest, AmpL4CoordinatorsShareIoTickWithoutOverwrite) {
  ASSERT_GE(sodium_init(), 0);

  auto clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  const auto addr = pp::adp::IpEndpoint::V4(10, 0, 0, 2, 1001);
  auto io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr);

  std::string peer_id;
  auto stack = MakeTestAmpStack(clock, io, &peer_id);
  ASSERT_NE(stack, nullptr);
  auto ma = pp::amp::FormatAdpMultiaddr(addr, peer_id);
  ASSERT_TRUE(static_cast<bool>(ma));

  MeshHost host;
  ASSERT_TRUE(static_cast<bool>(host.AttachAmpStack(std::move(stack), *ma)));
  ASSERT_NE(host.AmpCircuitClient(), nullptr);
  ASSERT_NE(host.AmpMediaRelayServer(), nullptr);

  host.AmpCircuitClient()->Start();
  host.AmpMediaRelayServer()->Start();
  EXPECT_TRUE(host.AmpCircuitClient()->IsStarted());
  EXPECT_TRUE(host.AmpMediaRelayServer()->IsStarted());

  // Both deadline ticks must remain registered (AddIoTick multiplex).
  host.Tick();
  EXPECT_TRUE(host.AmpCircuitClient()->IsStarted());
  EXPECT_TRUE(host.AmpMediaRelayServer()->IsStarted());

  host.AmpCircuitClient()->Stop();
  host.Tick();
  EXPECT_FALSE(host.AmpCircuitClient()->IsStarted());
  EXPECT_TRUE(host.AmpMediaRelayServer()->IsStarted());

  host.Stop();
}

// Connectivity needs Amp, not the media_relay client: with that client coordinator stopped, the dial
// registry still gets the Amp links and circuit reach is still built; only the media relay is out.
TEST(MeshHostAmpTest, ConnectivityWiresOnAmpWithoutTheMediaRelayClient) {
  ASSERT_GE(sodium_init(), 0);

  auto clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  const auto addr = pp::adp::IpEndpoint::V4(10, 0, 0, 3, 1002);
  auto io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr);

  std::string peer_id;
  auto stack = MakeTestAmpStack(clock, io, &peer_id);
  ASSERT_NE(stack, nullptr);
  auto ma = pp::amp::FormatAdpMultiaddr(addr, peer_id);
  ASSERT_TRUE(static_cast<bool>(ma));

  MeshHost host;
  ASSERT_TRUE(static_cast<bool>(host.AttachAmpStack(std::move(stack), *ma)));
  ASSERT_NE(host.AmpMediaRelayClientCoord(), nullptr);
  host.AmpMediaRelayClientCoord()->Stop();
  {
    MeshConnectivity connectivity;
    MeshMediaRelay media_relay(connectivity);
    MeshConnectivityDeps deps;
    deps.mesh = [&host]() { return &host; };
    connectivity.SetDeps(std::move(deps));
    connectivity.Wire();
    media_relay.Wire();

    ASSERT_NE(connectivity.Dial(), nullptr);
    const std::string peer_ma = "/ip4/203.0.113.9/udp/4001/adp/1.0.0/p2p/12D3KooWConnectivityPeer";
    EXPECT_TRUE(connectivity.Dial()->RegisterEndpoint("12D3KooWConnectivityPeer", peer_ma))
        << "the dial registry has the Amp links";
    EXPECT_NE(connectivity.CircuitReach(), nullptr) << "circuit reach needs only the circuit client";
    EXPECT_FALSE(media_relay.AmpRelayAvailable());
    EXPECT_EQ(media_relay.RelayClient(), nullptr);

    media_relay.Clear();
    connectivity.Clear();
  }
  host.Stop();
}

// Host A with a hot link to B; B then goes silent (its sends are dropped).
struct SilentPeerFixture {
  std::shared_ptr<pp::adp::VirtualClock> clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  std::shared_ptr<pp::adp::MemoryDatagramHub> hub = pp::adp::MemoryDatagramIo::MakeHub();
  std::shared_ptr<pp::adp::LossyDatagramIo> io_b;
  std::unique_ptr<pp::amp::AmpStack> stack_b;
  MeshHost host;
  std::vector<pp::amp::LinkEvent> events;

  void SetUp() {
    const auto addr_a = pp::adp::IpEndpoint::V4(10, 0, 1, 1, 1000);
    const auto addr_b = pp::adp::IpEndpoint::V4(10, 0, 1, 2, 2000);
    io_b = std::make_shared<pp::adp::LossyDatagramIo>(std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_b));
    std::string peer_a;
    std::string peer_b;
    auto stack_a = MakeTestAmpStack(clock, std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_a), &peer_a);
    stack_b = MakeTestAmpStack(clock, io_b, &peer_b);
    ASSERT_TRUE(stack_a && stack_b);
    stack_b->GetEndpoint().SetAcceptEnabled(true);
    stack_b->Start();
    auto ma_a = pp::amp::FormatAdpMultiaddr(addr_a, peer_a);
    auto ma_b = pp::amp::FormatAdpMultiaddr(addr_b, peer_b);
    ASSERT_TRUE(ma_a && ma_b);
    ASSERT_TRUE(static_cast<bool>(host.AttachAmpStack(std::move(stack_a), *ma_a)));
    auto& links = host.Amp()->Links();
    links.AddLinkEventListener([this](const pp::amp::LinkEvent& event) { events.push_back(event); });
    ASSERT_TRUE(static_cast<bool>(links.RegisterEndpoint("b", *ma_b)));
    bool connected = false;
    links.EnsureAssociation("b", [&](pp::amp::PeerLinkManager::LinkRoe r) { connected = static_cast<bool>(r); });
    Run(200, 10);
    ASSERT_TRUE(connected);
    links.MarkHot("b");  // hot: its liveness window is far longer than the test
    Run(10, 100);
    io_b->SetDropRate(1.0);
  }

  void Run(const int rounds, const int64_t step_ms) {
    for (int i = 0; i < rounds; ++i) {
      clock->Advance(step_ms);
      host.Tick();
      stack_b->Runtime().Drive();
    }
  }

  bool Dropped(const pp::amp::LinkDropReason reason) const {
    for (const auto& event : events) {
      if (event.kind == pp::amp::LinkEvent::Kind::Dropped && event.reason == reason) {
        return true;
      }
    }
    return false;
  }
};

// k5: after a network change the host's link to a peer that no longer answers is gone within
// Amp's 2 s grace — not after the hot link's liveness window.
TEST(MeshHostAmpTest, NetworkChangeEvictsADeadLinkFast) {
  ASSERT_GE(sodium_init(), 0);
  SilentPeerFixture f;
  f.SetUp();
  if (::testing::Test::HasFatalFailure()) {
    return;
  }
  // node-monitoring M2: the drop shows in the operator counters, by reason, as a live link.
  auto& drops = MetricsRegistry::Global().Counter("pp_link_drops_total",
                                                   "Amp links dropped, by reason; stage=attempt never connected.",
                                                   {{"reason", "network-changed"}, {"stage", "connected"}});
  const uint64_t drops_before = drops.Value();
  f.host.OnLocalNetworkChanged(LocalNetworkChange{true, true, true});
  f.Run(30, 100);  // 3 s
  EXPECT_TRUE(f.Dropped(pp::amp::LinkDropReason::NetworkChanged));
  EXPECT_EQ(f.host.Amp()->Links().FindLink("b"), nullptr);
  EXPECT_EQ(drops.Value(), drops_before + 1);
  // The link's reliable handshake was acked: round trips reached the histogram (M2 amp stats).
  const auto rtt = MetricsRegistry::Global()
                       .Histogram("pp_amp_rtt_seconds", "Amp round trips (acks of never-retransmitted reliable packets).",
                                  {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5})
                       .Read();
  EXPECT_GT(rtt.count, 0u);
  EXPECT_GT(f.host.Amp()->Runtime().GetEndpoint().Stats().tx_datagrams, 0u);
  f.host.Stop();
}

// Going offline probes nothing: links ride out a short outage (a later online change probes them).
TEST(MeshHostAmpTest, GoingOfflineLeavesLinksAlone) {
  ASSERT_GE(sodium_init(), 0);
  SilentPeerFixture f;
  f.SetUp();
  if (::testing::Test::HasFatalFailure()) {
    return;
  }
  f.host.OnLocalNetworkChanged(LocalNetworkChange{true, false, true});
  f.Run(30, 100);
  EXPECT_FALSE(f.Dropped(pp::amp::LinkDropReason::NetworkChanged));
  EXPECT_NE(f.host.Amp()->Links().FindLink("b"), nullptr);
  f.host.Stop();
}

TEST(LocalNetworkReactionTest, OnlyAMoveOnlineProbes) {
  // {was_online, online, attachment_changed}
  EXPECT_TRUE(DecideLocalNetworkReaction({true, true, true}).probe_links) << "new attachment";
  EXPECT_TRUE(DecideLocalNetworkReaction({false, true, false}).probe_links) << "back online";
  EXPECT_TRUE(DecideLocalNetworkReaction({false, true, false}).reprobe_reachability);
  EXPECT_FALSE(DecideLocalNetworkReaction({true, false, true}).probe_links) << "offline: nothing to probe through";
  EXPECT_FALSE(DecideLocalNetworkReaction({true, true, false}).probe_links) << "cost / label only";
  EXPECT_FALSE(DecideLocalNetworkReaction({true, true, false}).reprobe_reachability);
}

} // namespace
} // namespace pbr
