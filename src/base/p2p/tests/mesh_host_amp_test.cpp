#include "base/adp/Clock.h"
#include "base/adp/MemoryDatagramIo.h"
#include "base/crypto/MlDsa.h"
#include "base/mesh/link/AdpMultiaddr.h"
#include "base/mesh/link/AmpStack.h"
#include "base/mesh/link/tests/mesh_harness_support.h"
#include "base/p2p/AmpMediaRelayCoordinator.h"
#include "base/p2p/CircuitTunnelCoordinator.h"
#include "base/p2p/MeshHost.h"
#include "base/p2p/PeerIdUtil.h"

#include <gtest/gtest.h>
#include <sodium.h>

namespace pbr {
namespace {

std::unique_ptr<amp::AmpStack> MakeTestAmpStack(const std::shared_ptr<adp::Clock>& clock,
                                                const std::shared_ptr<adp::DatagramIo>& io,
                                                std::string* peer_id_out) {
  auto keys = MlDsa::GenerateKeyPair();
  if (!keys) {
    return nullptr;
  }
  amp::MshIdentity identity;
  identity.ml_dsa_secret_key = std::move(keys->secret_key);
  identity.ml_dsa_public_key = std::move(keys->public_key);
  auto peer_id = PeerIdFromMlDsaPublicKey(identity.ml_dsa_public_key);
  if (!peer_id) {
    return nullptr;
  }
  *peer_id_out = *peer_id;

  amp::AmpStack::Config cfg;
  cfg.identity = std::move(identity);
  cfg.local_peer_id = *peer_id;
  cfg.link_config = test::AmpMeshTestLinkConfig();

  auto stack = amp::AmpStack::Create(io, clock, cfg);
  if (!stack) {
    return nullptr;
  }
  return std::move(*stack);
}

TEST(MeshHostAmpTest, AttachAmpStackParallelNoLibp2p) {
  ASSERT_GE(sodium_init(), 0);

  auto clock = std::make_shared<adp::VirtualClock>(1'000'000);
  auto hub = adp::MemoryDatagramIo::MakeHub();
  const auto addr = adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
  auto io = std::make_shared<adp::MemoryDatagramIo>(hub, addr);

  std::string peer_id;
  auto stack = MakeTestAmpStack(clock, io, &peer_id);
  ASSERT_NE(stack, nullptr);
  auto ma = amp::FormatAdpMultiaddr(addr, peer_id);
  ASSERT_TRUE(static_cast<bool>(ma));

  MeshHost host;
  ASSERT_TRUE(static_cast<bool>(host.AttachAmpStack(std::move(stack), *ma)));
  ASSERT_NE(host.Amp(), nullptr);
  EXPECT_TRUE(host.Amp()->IsStarted());
  EXPECT_EQ(host.AmpListenMultiaddr(), *ma);
  EXPECT_EQ(host.Amp()->Links().LocalCapability().listen_multiaddrs, std::vector<std::string>{*ma});
  ASSERT_NE(host.AmpCircuitTunnel(), nullptr);
  ASSERT_NE(host.AmpMediaRelayCoord(), nullptr);
  EXPECT_TRUE(host.AmpCircuitTunnel()->IsStarted());
  EXPECT_TRUE(host.AmpMediaRelayCoord()->IsStarted());
  ASSERT_NE(host.AmpCircuitHops(), nullptr);

  host.Tick();
  host.Stop();
  EXPECT_EQ(host.Amp(), nullptr);
  EXPECT_EQ(host.AmpCircuitTunnel(), nullptr);
  EXPECT_EQ(host.AmpMediaRelayCoord(), nullptr);
  EXPECT_EQ(host.AmpCircuitHops(), nullptr);
  EXPECT_TRUE(host.AmpListenMultiaddr().empty());
}

TEST(MeshHostAmpTest, AmpL4CoordinatorsShareIoTickWithoutOverwrite) {
  ASSERT_GE(sodium_init(), 0);

  auto clock = std::make_shared<adp::VirtualClock>(1'000'000);
  auto hub = adp::MemoryDatagramIo::MakeHub();
  const auto addr = adp::IpEndpoint::V4(10, 0, 0, 2, 1001);
  auto io = std::make_shared<adp::MemoryDatagramIo>(hub, addr);

  std::string peer_id;
  auto stack = MakeTestAmpStack(clock, io, &peer_id);
  ASSERT_NE(stack, nullptr);
  auto ma = amp::FormatAdpMultiaddr(addr, peer_id);
  ASSERT_TRUE(static_cast<bool>(ma));

  MeshHost host;
  ASSERT_TRUE(static_cast<bool>(host.AttachAmpStack(std::move(stack), *ma)));
  ASSERT_NE(host.AmpCircuitTunnel(), nullptr);
  ASSERT_NE(host.AmpMediaRelayCoord(), nullptr);

  host.AmpCircuitTunnel()->Start();
  host.AmpMediaRelayCoord()->Start();
  EXPECT_TRUE(host.AmpCircuitTunnel()->IsStarted());
  EXPECT_TRUE(host.AmpMediaRelayCoord()->IsStarted());

  // Both deadline ticks must remain registered (AddIoTick multiplex).
  host.Tick();
  EXPECT_TRUE(host.AmpCircuitTunnel()->IsStarted());
  EXPECT_TRUE(host.AmpMediaRelayCoord()->IsStarted());

  host.AmpCircuitTunnel()->Stop();
  host.Tick();
  EXPECT_FALSE(host.AmpCircuitTunnel()->IsStarted());
  EXPECT_TRUE(host.AmpMediaRelayCoord()->IsStarted());

  host.Stop();
}

} // namespace
} // namespace pbr
