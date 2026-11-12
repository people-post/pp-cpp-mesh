#include "mesh/l4/media_relay/serve/MediaRelayAttachSm.h"
#include "mesh/l4/media_relay/MediaRelayErr.h"
#include "mesh/l4/media_relay/MediaRelayLogic.h"
#include "mesh/l4/media_relay/MediaRelayTypes.h"

#include <gtest/gtest.h>

namespace pp::mesh {
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

TEST(MediaRelayAttachSmTest, AttachOnlyAfterThisChannelsAccept) {
  // PB-021: attach follows this channel's own accept — its token is the one issued here. Joining an
  // existing session (a call-scoped guest, a reattach) quotes and accepts on its channel first.
  MediaRelayAttachSm sm;
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::StreamOpened));
  EXPECT_FALSE(sm.Apply(MediaRelayAttachEvent::OpAttach)) << "no attach straight from Control";
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::OpQuote));
  EXPECT_FALSE(sm.Apply(MediaRelayAttachEvent::OpAttach)) << "nor after a quote alone";
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::OpAccept));
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
  ASSERT_TRUE(sm.Apply(MediaRelayAttachEvent::OpAccept));
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

// A hop's refusal by its reply's code (docs/ERRORS.md), never its text: a missing or unknown code is a plain failure.
TEST(MediaRelayErrTest, AHopsRefusalReadsByItsCodeOnly) {
  EXPECT_EQ(MediaRelayErrFromCode("busy"), MediaRelayErr::HopBusy);
  EXPECT_EQ(MediaRelayErrFromCode("auth_failed"), MediaRelayErr::AuthFailed);
  EXPECT_EQ(MediaRelayErrFromCode("unknown_quote"), MediaRelayErr::UnknownQuote);
  EXPECT_EQ(MediaRelayErrFromCode("refused"), MediaRelayErr::HopRefused);
  EXPECT_EQ(MediaRelayErrFromCode("not_ready"), MediaRelayErr::HopRefused);
  EXPECT_EQ(MediaRelayErrFromCode("bad_request"), MediaRelayErr::HopRefused);
  EXPECT_EQ(MediaRelayErrFromCode(""), MediaRelayErr::Generic) << "no code: not classified from the text";
  EXPECT_EQ(MediaRelayErrFromCode("from_a_newer_hop"), MediaRelayErr::Generic);
}

} // namespace
} // namespace pp::mesh
