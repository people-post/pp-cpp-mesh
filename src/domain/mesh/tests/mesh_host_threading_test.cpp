#include "amp/L1/Clock.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "foundation/crypto/MlDsa.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "domain/mesh/tests/support/mesh_harness_support.h"
#include "domain/mesh/host/MeshControlDispatch.h"
#include "domain/mesh/host/MeshControlPool.h"
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
  EXPECT_TRUE(host.MeshControlRunning());
  EXPECT_TRUE(MeshControlDispatch::IsInstalled());

  std::atomic<bool> ran{false};
  host.PostControl([&ran]() { ran.store(true, std::memory_order_release); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!ran.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(ran.load(std::memory_order_acquire));

  host.Tick();
  host.Stop();
  EXPECT_FALSE(host.MeshControlRunning());
  EXPECT_FALSE(MeshControlDispatch::IsInstalled());
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

TEST(MeshControlPoolTest, PostAndShutdown) {
  MeshControlPool pool(2);
  std::atomic<int> n{0};
  for (int i = 0; i < 10; ++i) {
    pool.Post([&n]() { ++n; });
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (n.load(std::memory_order_acquire) < 10 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(n.load(std::memory_order_acquire), 10);
  pool.Shutdown();
  pool.Post([&n]() { ++n; }); // dropped after shutdown
  EXPECT_EQ(n.load(std::memory_order_acquire), 10);
}

TEST(MeshControlPoolTest, ShutdownBudgetDetachesStuckWorker) {
  MeshControlPool pool(1);
  std::mutex mu;
  std::condition_variable cv;
  bool release_worker = false;
  std::atomic<bool> entered{false};

  pool.Post([&]() {
    entered.store(true, std::memory_order_release);
    std::unique_lock lock(mu);
    cv.wait(lock, [&]() { return release_worker; });
  });

  const auto enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!entered.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < enter_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(entered.load(std::memory_order_acquire));

  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = pool.Shutdown(std::chrono::milliseconds(80));
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  EXPECT_FALSE(ok);
  EXPECT_LT(elapsed, std::chrono::milliseconds(500));

  {
    std::lock_guard lock(mu);
    release_worker = true;
  }
  cv.notify_all();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

} // namespace
} // namespace pbr
