#include "base/mesh/l4/media_relay/MediaRelayAttachSm.h"
#include "base/mesh/l4/media_relay/MediaRelayLogic.h"
#include "base/mesh/l4/media_relay/MediaRelayTypes.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(MediaRelayAttachSmTest, QuoteAcceptAttachHappyPath) {
  MediaRelayAttachSm sm;
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::StreamOpened));
  EXPECT_EQ(sm.phase, MediaRelayAttachPhase::Control);

  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::OpQuote));
  EXPECT_EQ(sm.phase, MediaRelayAttachPhase::Quoted);

  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::OpAccept));
  EXPECT_EQ(sm.phase, MediaRelayAttachPhase::Accepted);

  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::OpAttach));
  EXPECT_EQ(sm.phase, MediaRelayAttachPhase::Attaching);

  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::AttachOk));
  EXPECT_EQ(sm.phase, MediaRelayAttachPhase::Attached);
}

TEST(MediaRelayAttachSmTest, AttachFromControlWithoutQuote) {
  // Golden #2: attach may join an existing session without a prior quote on this stream.
  MediaRelayAttachSm sm;
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::StreamOpened));
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::OpAttach));
  EXPECT_EQ(sm.phase, MediaRelayAttachPhase::Attaching);
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::AttachOk));
  EXPECT_EQ(sm.phase, MediaRelayAttachPhase::Attached);
}

TEST(MediaRelayAttachSmTest, AdmitFailCloses) {
  MediaRelayAttachSm sm;
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::StreamOpened));
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::AdmitFail));
  EXPECT_EQ(sm.phase, MediaRelayAttachPhase::Closed);
}

TEST(MediaRelayAttachSmTest, RejectsAttachAfterAttached) {
  MediaRelayAttachSm sm;
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::StreamOpened));
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::OpAttach));
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::AttachOk));
  EXPECT_FALSE(sm.Apply(MediaRelayAttachEvent::OpAttach));
  EXPECT_EQ(sm.phase, MediaRelayAttachPhase::Attached);
}

TEST(MediaRelayClientPhaseTest, AttachHappyPath) {
  auto o = DecideMediaRelayClientPhase(MediaRelayClientPhase::Idle, MediaRelayClientEvent::AttachRequested);
  EXPECT_EQ(o.decision, MediaRelayClientPhaseDecision::Transition);
  EXPECT_EQ(o.next, MediaRelayClientPhase::Dialing);

  o = DecideMediaRelayClientPhase(MediaRelayClientPhase::Dialing, MediaRelayClientEvent::OpenStreamOk);
  EXPECT_EQ(o.decision, MediaRelayClientPhaseDecision::Transition);
  EXPECT_EQ(o.next, MediaRelayClientPhase::Accepting);

  o = DecideMediaRelayClientPhase(MediaRelayClientPhase::Accepting, MediaRelayClientEvent::AcceptOk);
  EXPECT_EQ(o.decision, MediaRelayClientPhaseDecision::Transition);
  EXPECT_EQ(o.next, MediaRelayClientPhase::Attaching);

  o = DecideMediaRelayClientPhase(MediaRelayClientPhase::Attaching, MediaRelayClientEvent::AttachOk);
  EXPECT_EQ(o.decision, MediaRelayClientPhaseDecision::Transition);
  EXPECT_EQ(o.next, MediaRelayClientPhase::Attached);
}

TEST(MediaRelayClientPhaseTest, DetachAbortsInFlight) {
  auto o = DecideMediaRelayClientPhase(MediaRelayClientPhase::Dialing, MediaRelayClientEvent::DetachRequested);
  EXPECT_EQ(o.decision, MediaRelayClientPhaseDecision::Transition);
  EXPECT_EQ(o.next, MediaRelayClientPhase::Idle);

  o = DecideMediaRelayClientPhase(MediaRelayClientPhase::Attached, MediaRelayClientEvent::DetachRequested);
  EXPECT_EQ(o.decision, MediaRelayClientPhaseDecision::Keep);
}

TEST(MediaRelayClientPhaseTest, DuplexLostIgnoredWhenIdle) {
  auto o = DecideMediaRelayClientPhase(MediaRelayClientPhase::Idle, MediaRelayClientEvent::DuplexLost);
  EXPECT_EQ(o.decision, MediaRelayClientPhaseDecision::Ignore);

  o = DecideMediaRelayClientPhase(MediaRelayClientPhase::Attached, MediaRelayClientEvent::DuplexLost);
  EXPECT_EQ(o.decision, MediaRelayClientPhaseDecision::Transition);
  EXPECT_EQ(o.next, MediaRelayClientPhase::Idle);
}

TEST(MediaRelayClientPhaseTest, OpenStreamOkIgnoredOutsideDialing) {
  auto o = DecideMediaRelayClientPhase(MediaRelayClientPhase::Accepting, MediaRelayClientEvent::OpenStreamOk);
  EXPECT_EQ(o.decision, MediaRelayClientPhaseDecision::Ignore);
}

TEST(MediaRelayLogicTest, AuthStubRequiresNonEmptyCallIdMatch) {
  EXPECT_FALSE(MediaRelayAuthStubOk("", "call:1"));
  EXPECT_FALSE(MediaRelayAuthStubOk("other", "call:1"));
  EXPECT_TRUE(MediaRelayAuthStubOk("call:1", "call:1"));
}

TEST(MediaRelayLogicTest, StaleLossyDrop) {
  EXPECT_FALSE(ShouldDropStaleLossyFrame(false, 0, 1, 0));
  EXPECT_FALSE(ShouldDropStaleLossyFrame(true, 5, 6, 0));
  EXPECT_TRUE(ShouldDropStaleLossyFrame(true, 5, 4, 0));
  EXPECT_FALSE(ShouldDropStaleLossyFrame(true, 5, 4, 1)); // mark keeps IDR
}

TEST(MediaRelayLogicTest, CallScopedAdmitAndCaps) {
  EXPECT_TRUE(MediaRelayCallScopedAdmit(true, false));
  EXPECT_TRUE(MediaRelayCallScopedAdmit(false, true));
  EXPECT_FALSE(MediaRelayCallScopedAdmit(false, false));

  EXPECT_TRUE(MediaRelayCanOpenHostSession(3, kMediaRelayMaxHostSessions));
  EXPECT_FALSE(MediaRelayCanOpenHostSession(4, kMediaRelayMaxHostSessions));
  EXPECT_TRUE(MediaRelayCanAddParticipant(7, kMediaRelayMaxParticipantsPerSession));
  EXPECT_FALSE(MediaRelayCanAddParticipant(8, kMediaRelayMaxParticipantsPerSession));
}

} // namespace
} // namespace pbr
