#include "amp/L1/Clock.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "crypto/MlDsa.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "mesh/testing/mesh_harness_support.h"
#include "mesh/host/MeshHost.h"
#include "mesh/host/MeshPumpThread.h"
#include "crypto/PeerId.h"

#include <gtest/gtest.h>
#include <future>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sodium.h>
#include <thread>

namespace pp::mesh {
namespace {

std::unique_ptr<pp::amp::AmpStack> MakeTestAmpStack(const std::shared_ptr<pp::adp::Clock>& clock,
                                                    const std::shared_ptr<pp::adp::DatagramIo>& io,
                                                    std::string* peer_id_out) {
  auto keys = pp::MlDsa::GenerateKeyPair();
  if (!keys) {
    return nullptr;
  }
  pp::amp::MshIdentity identity;
  identity.ml_dsa_secret_key = std::move(keys->secret_key);
  identity.ml_dsa_public_key = std::move(keys->public_key);
  auto peer_id = pp::PeerIdFromMlDsaPublicKey(identity.ml_dsa_public_key);
  if (!peer_id) {
    return nullptr;
  }
  *peer_id_out = *peer_id;

  pp::amp::AmpStack::Config cfg;
  cfg.identity = std::move(identity);
  cfg.local_peer_id = *peer_id;
  cfg.link_config = pp::mesh::test::AmpMeshTestLinkConfig();

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

  MeshHost host{MeshExecutors{}, MetricsRegistry::Global()}; // manual drive: L4 work inline
  ASSERT_TRUE(static_cast<bool>(host.AttachAmpStack(std::move(stack), *ma)));
  EXPECT_FALSE(host.MeshPumpRunning());

  host.Tick();
  host.Stop();
  host.Stop(); // idempotent
}

// docs/THREADING.md: MeshPump is the runtime's only driver. pp-node's main loop used to
// Tick beside it — two drivers, and the DHT's self-publish raced the io lane. A drive from another
// thread is now refused (counted), and the io lane's work runs on the pump.
TEST(MeshHostThreadingTest, MeshPumpIsTheOnlyDriver) {
  ASSERT_GE(sodium_init(), 0);

  auto clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  const auto addr = pp::adp::IpEndpoint::V4(10, 0, 0, 3, 1000);
  auto io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr);

  std::string peer_id;
  auto stack = MakeTestAmpStack(clock, io, &peer_id);
  ASSERT_NE(stack, nullptr);
  auto ma = pp::amp::FormatAdpMultiaddr(addr, peer_id);
  ASSERT_TRUE(static_cast<bool>(ma));

  MeshHost host{MeshExecutors{}, MetricsRegistry::Global()};
  ASSERT_TRUE(static_cast<bool>(host.AttachAmpStack(std::move(stack), *ma, MeshHost::AttachDrive::MeshPump)));
  ASSERT_TRUE(host.MeshPumpRunning());
  auto& runtime = host.Amp()->Runtime();

  std::promise<bool> on_pump;
  host.IoLane()->Post([&]() { on_pump.set_value(host.IoLane()->RunsTasksOnCurrentThread()); });
  auto ran = on_pump.get_future();
  ASSERT_EQ(ran.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_TRUE(ran.get()) << "the lane's work runs on the pump";

  const uint64_t refused = runtime.RefusedDrives();
  host.Tick(); // a second driver
  EXPECT_EQ(runtime.RefusedDrives(), refused + 1);
  host.Stop();
}

// A tick that says more is waiting is followed by the next at once, not after the interval: a fixed nap after every
// tick capped what a node could take in, and the kernel dropped the rest (pp-browser stress lab, 2026-11-06).
TEST(MeshPumpThreadTest, AFullDrainIsFollowedAtOnce) {
  std::atomic<int> ticks{0};
  MeshPumpThread pump;
  pump.Start(
      [&ticks]() {
        ++ticks;
        return true; // always more waiting
      },
      std::chrono::milliseconds(50));
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  pump.Stop();
  EXPECT_GT(ticks.load(), 20) << "an idle pump would have ticked about 3 times";
}

// The busy share is the part of a second spent in ticks: near 1 for a pump that never catches up, near 0 when idle.
TEST(MeshPumpThreadTest, TheBusyShareFollowsTheTicks) {
  MeshPumpThread busy;
  busy.Start(
      []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return true;
      },
      std::chrono::milliseconds(5));
  MeshPumpThread idle;
  idle.Start([]() { return false; }, std::chrono::milliseconds(5));
  std::this_thread::sleep_for(std::chrono::milliseconds(2300));
  EXPECT_GT(busy.BusyFraction(), 0.8); // near 1; a scheduler's yield takes its share (0.89 on macos-15)
  EXPECT_LT(idle.BusyFraction(), 0.2);
  busy.Stop();
  idle.Stop();
}

TEST(MeshPumpThreadTest, StartStopJoins) {
  std::atomic<int> ticks{0};
  MeshPumpThread pump;
  pump.Start(
      [&ticks]() {
        ++ticks;
        return false;
      },
      std::chrono::milliseconds(5));
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

// An idle pump waits the way its host says (the socket's readiness), capped by its interval, and Stop ends that
// wait at once: a long wait never holds a Stop.
TEST(MeshPumpThreadTest, AnIdleWaitIsTheHostsAndStopEndsIt) {
  std::mutex mu;
  std::condition_variable cv;
  bool woken = false;
  std::atomic<int> waits{0};
  std::atomic<int64_t> asked_ms{0};
  MeshPumpThread pump;
  pump.SetIdleWait(
      [&](const std::chrono::milliseconds most) {
        asked_ms.store(most.count());
        waits.fetch_add(1);
        std::unique_lock lock(mu);
        cv.wait_for(lock, most, [&] { return woken; });
        return false;
      },
      [&] {
        {
          std::lock_guard lock(mu);
          woken = true;
        }
        cv.notify_all();
      });
  pump.Start([] { return false; }, std::chrono::milliseconds(10000));
  for (int i = 0; i < 200 && waits.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GE(waits.load(), 1) << "an idle pump waited the host's way";
  EXPECT_EQ(asked_ms.load(), 10000) << "capped by its interval";
  const auto began = std::chrono::steady_clock::now();
  pump.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(2)) << "Stop woke the wait";
}

/** A tick for the pump tests: counts, and says what a drive would. */
struct FakeTicks {
  std::atomic<int> ticks{0};
  std::atomic<int64_t> next_due_ms{200};
  std::atomic<bool> worked{false};

  MeshPumpThread::Turn operator()() {
    ticks.fetch_add(1);
    MeshPumpThread::Turn turn;
    turn.worked = worked.load();
    turn.next_due = std::chrono::milliseconds(next_due_ms.load());
    return turn;
  }
};

int TicksAfter(const FakeTicks& fake, const std::chrono::milliseconds span) {
  const int before = fake.ticks.load();
  std::this_thread::sleep_for(span);
  return fake.ticks.load() - before;
}

// An idle pump waits until the tick's next deadline, not 5 ms: an idle phone woke 200 times a second for nothing
// (pp-browser #596). Nothing due: the cap.
TEST(MeshPumpThreadTest, AnIdlePumpWaitsUntilTheNextDeadline) {
  FakeTicks fake;
  std::atomic<int64_t> asked_ms{0};
  MeshPumpThread pump;
  pump.SetIdleWait(
      [&](const std::chrono::milliseconds most) {
        asked_ms.store(most.count());
        std::this_thread::sleep_for(most);
        return false;
      },
      [] {});
  pump.Start([&fake] { return fake(); }, std::chrono::milliseconds(1000));
  const int ticks = TicksAfter(fake, std::chrono::milliseconds(1000));
  EXPECT_GE(ticks, 3);
  EXPECT_LE(ticks, 7) << "about one per 200 ms deadline, not 200 a second";
  EXPECT_EQ(asked_ms.load(), 200) << "the wait is the deadline";
  fake.next_due_ms.store(std::chrono::milliseconds::max().count());
  (void)TicksAfter(fake, std::chrono::milliseconds(300));
  EXPECT_EQ(asked_ms.load(), 1000) << "nothing due: the cap";
  EXPECT_EQ(pump.SpinGuards(), 0u);
  pump.Stop();
}

// Work another thread puts on the lane wakes the pump at once (MeshRuntime's wake handler calls Wake), through the
// host's wait and through the pump's own.
TEST(MeshPumpThreadTest, AWakeEndsTheWaitAtOnce) {
  for (const bool host_wait : {true, false}) {
    FakeTicks fake;
    fake.next_due_ms.store(10'000);
    fake.worked.store(true);
    std::mutex mu;
    std::condition_variable cv;
    bool woken = false;
    MeshPumpThread pump;
    if (host_wait) {
      pump.SetIdleWait(
          [&](const std::chrono::milliseconds most) {
            std::unique_lock lock(mu);
            cv.wait_for(lock, most, [&] { return woken; });
            woken = false;
            return false;
          },
          [&] {
            {
              std::lock_guard lock(mu);
              woken = true;
            }
            cv.notify_all();
          });
    }
    pump.Start([&fake] { return fake(); }, std::chrono::milliseconds(10'000));
    for (int i = 0; i < 200 && fake.ticks.load() == 0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // waiting now
    const int before = fake.ticks.load();
    const auto began = std::chrono::steady_clock::now();
    pump.Wake();
    while (fake.ticks.load() == before && std::chrono::steady_clock::now() - began < std::chrono::seconds(5)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::milliseconds(500)) << "host wait: " << host_wait;
    pump.Stop();
  }
}

// A wait that ends at once for nothing (a socket "ready" for ever with nothing to read) followed by a tick that reads
// nothing is not repeated: the pump stops polling for a while and waits plainly. It spun at 100 % of a core (#596).
TEST(MeshPumpThreadTest, AWaitThatEndsAtOnceForNothingDoesNotSpin) {
  FakeTicks fake;
  fake.next_due_ms.store(100);
  std::atomic<int> polls{0};
  MeshPumpThread pump;
  pump.SetIdleWait(
      [&](std::chrono::milliseconds) {
        polls.fetch_add(1);
        return true; // "readable", at once, every time
      },
      [] {});
  pump.Start([&fake] { return fake(); }, std::chrono::milliseconds(1000));
  const int ticks = TicksAfter(fake, std::chrono::milliseconds(1000));
  EXPECT_LE(ticks, 30) << "about one per 100 ms deadline, a few polls between guards — not a spin";
  EXPECT_GE(pump.SpinGuards(), 1u);
  EXPECT_LE(polls.load(), 12);
  pump.Stop();
}

// A tick that keeps saying "due now" and does nothing does not make a busy loop either: the guard's waits are at
// least the old 5 ms.
TEST(MeshPumpThreadTest, ATickDueNowThatDoesNothingDoesNotSpin) {
  FakeTicks fake;
  fake.next_due_ms.store(0);
  MeshPumpThread pump;
  pump.Start([&fake] { return fake(); }, std::chrono::milliseconds(1000));
  const int ticks = TicksAfter(fake, std::chrono::milliseconds(1000));
  EXPECT_LE(ticks, 300) << "at most the old 5 ms cadence";
  EXPECT_GE(pump.SpinGuards(), 1u);
  pump.Stop();
}

} // namespace
} // namespace pp::mesh
