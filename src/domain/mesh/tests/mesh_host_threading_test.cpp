#include "amp/L1/Clock.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "foundation/crypto/MlDsa.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "domain/mesh/tests/support/mesh_harness_support.h"
#include "domain/mesh/host/MeshHost.h"
#include "domain/mesh/host/MeshPumpThread.h"
#include "foundation/identity/PeerIdUtil.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sodium.h>
#include <thread>

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

TEST(MeshHostThreadingTest, AttachInstallsControlWithoutPump) {
  ASSERT_GE(sodium_init(), 0);

  auto clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  const auto addr = pp::adp::IpEndpoint::V4(10, 0, 0, 2, 1000);
  auto io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr);

  std::string peer_id;
  auto stack = MakeTestAmpStack(clock, io, &peer_id);
  ASSERT_NE(stack, nullptr);
  auto ma = pp::amp::FormatAdpMultiaddr(addr, peer_id);
  ASSERT_TRUE(static_cast<bool>(ma));

  MeshHost host;
  ASSERT_TRUE(static_cast<bool>(host.AttachAmpStack(std::move(stack), *ma)));
  EXPECT_FALSE(host.MeshPumpRunning());

  host.Tick();
  host.Stop();
  host.Stop(); // idempotent
}

TEST(MeshPumpThreadTest, StartStopJoins) {
  std::atomic<int> ticks{0};
  MeshPumpThread pump;
  pump.Start([&ticks]() { ++ticks; }, std::chrono::milliseconds(5));
  EXPECT_TRUE(pump.IsRunning());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (ticks.load(std::memory_order_acquire) < 3 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_GE(ticks.load(std::memory_order_acquire), 3);
  pump.Stop();
  EXPECT_FALSE(pump.IsRunning());
  pump.Stop();
}

} // namespace
} // namespace pbr
