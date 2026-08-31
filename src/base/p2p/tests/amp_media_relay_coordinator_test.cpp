#include "base/p2p/AmpMediaRelayCoordinator.h"
#include "base/mesh/link/tests/mesh_test_harness.h"
#include "base/people/RelayScope.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>

namespace pbr {
namespace {

class AmpMediaRelayCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    harness_ = std::move(*created);
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("hop", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b().RegisterEndpoint("client", harness_->ma_a)));

    hop_ = std::make_unique<AmpMediaRelayCoordinator>(*harness_->runtime_b);
    client_ = std::make_unique<AmpMediaRelayCoordinator>(*harness_->runtime_a);
    hop_->Start();
    client_->Start();
  }

  void TearDown() override {
    if (client_) {
      client_->Stop();
    }
    if (hop_) {
      hop_->Stop();
    }
    client_.reset();
    hop_.reset();
    harness_.reset();
  }

  template <typename Result>
  struct Wait {
    std::atomic<bool> done{false};
    Roe<Result> result = Error("pending");

    std::function<void(Roe<Result>)> Fn() {
      return [this](Roe<Result> r) {
        result = std::move(r);
        done.store(true, std::memory_order_release);
      };
    }

    void PumpUntilDone(test::AmpMeshHarness& harness, const size_t max_rounds = 800) {
      harness.PumpUntil([this] { return done.load(std::memory_order_acquire); }, max_rounds);
      ASSERT_TRUE(done.load(std::memory_order_acquire)) << "media-relay completion timed out";
    }
  };

  std::unique_ptr<test::AmpMeshHarness> harness_;
  std::unique_ptr<AmpMediaRelayCoordinator> hop_;
  std::unique_ptr<AmpMediaRelayCoordinator> client_;
};

TEST_F(AmpMediaRelayCoordinatorTest, QuoteRoundTrip) {
  MediaRelayQuoteRequest req;
  req.call_id = "call-amp-quote";
  req.participants = 2;

  Wait<MediaRelayQuote> wait;
  auto id = client_->StartQuote("hop", req, wait.Fn(), 8000);
  ASSERT_TRUE(id);
  wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(wait.result) << wait.result.error().message;
  EXPECT_TRUE(wait.result->ok);
  EXPECT_FALSE(wait.result->quote_id.empty());
  EXPECT_EQ(wait.result->pricing_mode, "volunteer");
}

TEST_F(AmpMediaRelayCoordinatorTest, AcceptAndAttachRoundTrip) {
  MediaRelayQuoteRequest req;
  req.call_id = "call-amp-attach";

  Wait<MediaRelayQuote> quote_wait;
  auto qid = client_->StartQuote("hop", req, quote_wait.Fn(), 8000);
  ASSERT_TRUE(qid);
  quote_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(quote_wait.result) << quote_wait.result.error().message;
  ASSERT_TRUE(quote_wait.result->ok);

  Wait<MediaRelayAttachResult> attach_wait;
  auto aid = client_->StartAttach("hop", quote_wait.result->quote_id, req.call_id, req.call_id, {},
                                  attach_wait.Fn(), 8000);
  ASSERT_TRUE(aid);
  attach_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(attach_wait.result) << attach_wait.result.error().message;
  EXPECT_TRUE(attach_wait.result->ok);
  EXPECT_FALSE(attach_wait.result->session_token.empty());
  EXPECT_EQ(client_->Phase(aid), MediaRelayBundlePhase::Attached);
}

TEST_F(AmpMediaRelayCoordinatorTest, AdmitRefusesStrangerOnQuote) {
  MediaRelayAdmissionPolicy policy;
  policy.serve_scope_mask = kRelayScopeLinkSiteSocial;
  policy.contact_peer_ids = {"not-the-client"};
  hop_->SetAdmissionPolicy(std::move(policy));

  MediaRelayQuoteRequest req;
  req.call_id = "call-stranger";
  Wait<MediaRelayQuote> wait;
  auto id = client_->StartQuote("hop", req, wait.Fn(), 5000);
  ASSERT_TRUE(id);
  wait.PumpUntilDone(*harness_);
  ASSERT_FALSE(wait.result);
  EXPECT_NE(wait.result.error().message.find("stranger"), std::string::npos);
}

} // namespace
} // namespace pbr
