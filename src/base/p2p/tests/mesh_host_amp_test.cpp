#include "base/adp/Clock.h"
#include "base/adp/MemoryDatagramIo.h"
#include "base/crypto/MlDsa.h"
#include "base/mesh/link/AdpMultiaddr.h"
#include "base/mesh/link/AmpStack.h"
#include "base/mesh/link/tests/mesh_harness_support.h"
#include "base/p2p/MeshHost.h"
#include "base/p2p/PeerIdUtil.h"

#include <gtest/gtest.h>
#include <sodium.h>

namespace pbr {
namespace {

TEST(MeshHostAmpTest, AttachAmpStackParallelNoLibp2p) {
  ASSERT_GE(sodium_init(), 0);

  auto clock = std::make_shared<adp::VirtualClock>(1'000'000);
  auto hub = adp::MemoryDatagramIo::MakeHub();
  const auto addr = adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
  auto io = std::make_shared<adp::MemoryDatagramIo>(hub, addr);

  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  amp::MshIdentity identity;
  identity.ml_dsa_secret_key = std::move(keys->secret_key);
  identity.ml_dsa_public_key = std::move(keys->public_key);
  auto peer_id = PeerIdFromMlDsaPublicKey(identity.ml_dsa_public_key);
  ASSERT_TRUE(static_cast<bool>(peer_id));

  amp::AmpStack::Config cfg;
  cfg.identity = std::move(identity);
  cfg.local_peer_id = *peer_id;
  cfg.link_config = test::AmpMeshTestLinkConfig();

  auto stack = amp::AmpStack::Create(io, clock, cfg);
  ASSERT_TRUE(static_cast<bool>(stack));
  auto ma = amp::FormatAdpMultiaddr(addr, *peer_id);
  ASSERT_TRUE(static_cast<bool>(ma));

  MeshHost host;
  ASSERT_TRUE(static_cast<bool>(host.AttachAmpStack(std::move(*stack), *ma)));
  ASSERT_NE(host.Amp(), nullptr);
  EXPECT_TRUE(host.Amp()->IsStarted());
  EXPECT_EQ(host.AmpListenMultiaddr(), *ma);
  EXPECT_EQ(host.Amp()->Links().LocalCapability().listen_multiaddrs, std::vector<std::string>{*ma});

  host.Tick();
  host.Stop();
  EXPECT_EQ(host.Amp(), nullptr);
  EXPECT_TRUE(host.AmpListenMultiaddr().empty());
}

} // namespace
} // namespace pbr
