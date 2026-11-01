#include "amp/L1/Clock.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "crypto/MlDsa.h"
#include "crypto/PeerId.h"
#include "mesh/host/MeshHost.h"
#include "mesh/host/UdpSocketProbe.h"
#include "mesh/testing/mesh_harness_support.h"

#include <gtest/gtest.h>
#include <optional>
#include <sodium.h>
#include <thread>

namespace pp::mesh {
namespace {

// iOS's dead socket after a suspend (#416): sends leave, nothing is ever received. A reopen heals it
// when `heals`; otherwise it is refused as by in-memory IO.
class DeafDatagramIo final : public pp::adp::DatagramIo {
public:
  DeafDatagramIo(std::shared_ptr<pp::adp::DatagramIo> inner, bool heals) : inner_(std::move(inner)), heals_(heals) {}
  Roe<void> SendTo(const pp::adp::IpEndpoint& peer, std::span<const uint8_t> datagram) override {
    return inner_->SendTo(peer, datagram);
  }
  Roe<std::optional<std::pair<pp::adp::IpEndpoint, std::vector<uint8_t>>>> RecvFrom() override {
    if (deaf_) {
      return Error("recvfrom errno=57");
    }
    return inner_->RecvFrom();
  }
  pp::adp::IpEndpoint LocalEndpoint() const override { return inner_->LocalEndpoint(); }
  Roe<void> Reopen() override {
    if (!heals_) {
      return inner_->Reopen();
    }
    deaf_ = false;
    ++reopens_;
    return {};
  }
  int reopens_ = 0;

private:
  std::shared_ptr<pp::adp::DatagramIo> inner_;
  bool heals_ = false;
  bool deaf_ = true;
};

class UdpSocketProbeTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }

  void Attach(const std::shared_ptr<pp::adp::DatagramIo>& io) { Attach(io, clock_); }

  void Attach(const std::shared_ptr<pp::adp::DatagramIo>& io, const std::shared_ptr<pp::adp::Clock>& clock) {
    auto keys = pp::MlDsa::GenerateKeyPair();
    ASSERT_TRUE(static_cast<bool>(keys));
    pp::amp::MshIdentity identity;
    identity.ml_dsa_secret_key = std::move(keys->secret_key);
    identity.ml_dsa_public_key = std::move(keys->public_key);
    auto peer_id = pp::PeerIdFromMlDsaPublicKey(identity.ml_dsa_public_key);
    ASSERT_TRUE(static_cast<bool>(peer_id));
    pp::amp::AmpStack::Config cfg;
    cfg.identity = std::move(identity);
    cfg.local_peer_id = *peer_id;
    cfg.link_config = pp::mesh::test::AmpMeshTestLinkConfig();
    auto stack = pp::amp::AmpStack::Create(io, clock, cfg);
    ASSERT_TRUE(static_cast<bool>(stack));
    auto ma = pp::amp::FormatAdpMultiaddr(io->LocalEndpoint(), *peer_id);
    ASSERT_TRUE(static_cast<bool>(ma));
    ASSERT_TRUE(static_cast<bool>(host_.AttachAmpStack(std::move(*stack), *ma)));
  }

  /** Drives the host (manual drive) for `ms` of Amp time, or until the probe answered. */
  void DriveFor(int64_t ms) {
    for (int64_t t = 0; t <= ms && !result_; t += 50) {
      host_.Tick();
      clock_->Advance(50);
    }
    host_.Tick();
  }

  void Probe() {
    host_.ProbeUdpSocket([this](const UdpSocketProbeResult& r) { result_ = r; });
  }

  /** EnsureUdpSocketAlive, driven until it answered. */
  std::optional<UdpSocketCheck> Ensure() {
    std::optional<UdpSocketCheck> check;
    host_.SetOnUdpSocketCheck([&check](const UdpSocketCheck& c) { check = c; });
    host_.EnsureUdpSocketAlive(UdpSocketCheckReason::Requested);
    for (int i = 0; i < 100 && !check; ++i) {
      host_.Tick();
      clock_->Advance(50);
    }
    host_.SetOnUdpSocketCheck({});
    return check;
  }

  void TearDown() override { host_.Stop(); }

  std::shared_ptr<pp::adp::VirtualClock> clock_ = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  std::shared_ptr<pp::adp::MemoryDatagramHub> hub_ = pp::adp::MemoryDatagramIo::MakeHub();
  const pp::adp::IpEndpoint addr_ = pp::adp::IpEndpoint::V4(10, 0, 0, 9, 1000);
  MeshHost host_{MeshExecutors{}, MetricsRegistry::Global()};
  std::optional<UdpSocketProbeResult> result_;
};

TEST_F(UdpSocketProbeTest, ALiveSocketHearsItself) {
  Attach(std::make_shared<pp::adp::MemoryDatagramIo>(hub_, addr_));
  Probe();
  DriveFor(2000);
  ASSERT_TRUE(result_.has_value());
  EXPECT_TRUE(result_->ok) << result_->error;
}

TEST_F(UdpSocketProbeTest, ASocketThatCannotSendIsDeadAtOnce) {
  auto io = std::make_shared<pp::adp::MemoryDatagramIo>(hub_, addr_);
  io->SetUnreachable(addr_, true);
  Attach(io);
  Probe();
  host_.Tick();
  ASSERT_TRUE(result_.has_value()) << "a failed send needs no wait";
  EXPECT_FALSE(result_->ok);
  EXPECT_NE(result_->error.find("unreachable"), std::string::npos) << result_->error;
}

TEST_F(UdpSocketProbeTest, ASocketThatHearsNothingIsDeadAfterTwoTries) {
  Attach(std::make_shared<DeafDatagramIo>(std::make_shared<pp::adp::MemoryDatagramIo>(hub_, addr_), false));
  Probe();
  DriveFor((kUdpSocketProbeFirstWait + kUdpSocketProbeSecondWait).count() - 100);
  EXPECT_FALSE(result_.has_value()) << "one quiet window is not enough";
  DriveFor(500);
  ASSERT_TRUE(result_.has_value());
  EXPECT_FALSE(result_->ok);
  EXPECT_EQ(result_->error, "nothing received");
}

TEST_F(UdpSocketProbeTest, ALiveSocketIsLeftAlone) {
  auto io = std::make_shared<DeafDatagramIo>(std::make_shared<pp::adp::MemoryDatagramIo>(hub_, addr_), true);
  ASSERT_TRUE(static_cast<bool>(io->Reopen()));  // hears from the start
  Attach(io);
  const auto check = Ensure();
  ASSERT_TRUE(check.has_value());
  EXPECT_TRUE(check->was_alive);
  EXPECT_TRUE(check->alive);
  EXPECT_FALSE(check->reopened);
  EXPECT_EQ(io->reopens_, 1) << "no reopen beyond the test's own";
}

TEST_F(UdpSocketProbeTest, ADeadSocketIsReopenedAndHearsAgain) {
  auto io = std::make_shared<DeafDatagramIo>(std::make_shared<pp::adp::MemoryDatagramIo>(hub_, addr_), true);
  Attach(io);
  const auto check = Ensure();
  ASSERT_TRUE(check.has_value());
  EXPECT_FALSE(check->was_alive);
  EXPECT_TRUE(check->reopened);
  EXPECT_TRUE(check->alive) << check->error;
  EXPECT_EQ(check->error, "nothing received");
  EXPECT_EQ(io->reopens_, 1);
}

TEST_F(UdpSocketProbeTest, ASocketThatCannotBeReopenedStaysDeadAndSaysWhy) {
  Attach(std::make_shared<DeafDatagramIo>(std::make_shared<pp::adp::MemoryDatagramIo>(hub_, addr_), false));
  const auto check = Ensure();
  ASSERT_TRUE(check.has_value());
  EXPECT_FALSE(check->reopened);
  EXPECT_FALSE(check->alive);
  EXPECT_NE(check->error.find("reopen failed"), std::string::npos) << check->error;
}

TEST_F(UdpSocketProbeTest, ANetworkChangeChecksTheSocketAndReopensADeadOne) {
  auto io = std::make_shared<DeafDatagramIo>(std::make_shared<pp::adp::MemoryDatagramIo>(hub_, addr_), true);
  Attach(io);
  std::optional<UdpSocketCheck> check;
  host_.SetOnUdpSocketCheck([&check](const UdpSocketCheck& c) { check = c; });
  host_.OnLocalNetworkChanged(LocalNetworkChange{.was_online = true, .online = true, .attachment_changed = true});
  for (int i = 0; i < 100 && !check; ++i) {
    host_.Tick();
    clock_->Advance(50);
  }
  ASSERT_TRUE(check.has_value());
  EXPECT_EQ(check->reason, UdpSocketCheckReason::NetworkChange);
  EXPECT_TRUE(check->reopened);
  EXPECT_TRUE(check->alive) << check->error;
  host_.SetOnUdpSocketCheck({});
}

// The product's socket: dual-stack, bound to any — the probe goes to [::1] on its own port.
TEST_F(UdpSocketProbeTest, AnOsSocketBoundToAnyHearsItselfOverLoopback) {
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V6({}, 0));
  if (!bound) {
    bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(0, 0, 0, 0, 0));
  }
  ASSERT_TRUE(static_cast<bool>(bound)) << bound.error().message;
  Attach(std::shared_ptr<pp::adp::DatagramIo>(std::move(*bound)), std::make_shared<pp::adp::WallClock>());
  Probe();
  for (int i = 0; i < 300 && !result_; ++i) {
    host_.Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(result_.has_value());
  EXPECT_TRUE(result_->ok) << result_->error;
}

TEST(UdpSocketProbeTargetTest, AnUnboundAddressProbesLoopbackOfItsFamily) {
  EXPECT_EQ(UdpSocketProbeTarget(pp::adp::IpEndpoint::V4(0, 0, 0, 0, 4001)),
            pp::adp::IpEndpoint::V4(127, 0, 0, 1, 4001));
  std::array<uint8_t, 16> loopback{};
  loopback[15] = 1;
  EXPECT_EQ(UdpSocketProbeTarget(pp::adp::IpEndpoint::V6({}, 4001)), pp::adp::IpEndpoint::V6(loopback, 4001));
  const auto bound = pp::adp::IpEndpoint::V4(192, 168, 0, 7, 4001);
  EXPECT_EQ(UdpSocketProbeTarget(bound), bound);
}

} // namespace
} // namespace pp::mesh
