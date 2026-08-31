#pragma once

#include "base/p2p/MediaRelayTypes.h"
#include "base/people/RelayScope.h"

#include <cstdint>
#include <string>
#include <unordered_set>

namespace pbr {

struct MediaRelaySessionId {
  uint64_t value = 0;
  explicit operator bool() const { return value != 0; }
  friend bool operator==(MediaRelaySessionId a, MediaRelaySessionId b) { return a.value == b.value; }
};

/** AMP-native media-relay session phase (client or hop serve). */
enum class MediaRelayBundlePhase {
  Idle = 0,
  OutboundQuote,
  WaitQuote,
  OutboundAccept,
  WaitAccept,
  OutboundAttach,
  WaitAttachAck,
  Attached,
  HostServe,
  Closing,
};

enum class MediaRelayBundleRole {
  ClientQuote = 0,
  ClientAttach,
  HopServe,
};

enum class MediaRelayOpAdmitDecision {
  Allow = 0,
  RefuseStranger,
  RefuseNotReady,
  RefuseBadOp,
};

struct MediaRelayOpAdmitContext {
  bool service_started = false;
  bool stopping = false;
  std::string dialer_peer_id;
  std::string op;
  std::string call_id;
  bool session_exists_for_call = false;
  RelayScopeMask serve_scope_mask = kRelayScopeVolunteerServe;
  std::unordered_set<std::string> contact_peer_ids;
};

MediaRelayOpAdmitDecision DecideMediaRelayOpAdmit(const MediaRelayOpAdmitContext& ctx);

enum class MediaRelayQuoteAckDecision {
  Succeed = 0,
  Fail,
  IgnoreStale,
};

struct MediaRelayQuoteAckContext {
  MediaRelayBundlePhase phase = MediaRelayBundlePhase::Idle;
  bool ack_ok = false;
};

MediaRelayQuoteAckDecision DecideMediaRelayQuoteAck(const MediaRelayQuoteAckContext& ctx);

enum class MediaRelayAttachAckDecision {
  EnterAttached = 0,
  Fail,
  IgnoreStale,
};

struct MediaRelayAttachAckContext {
  MediaRelayBundlePhase phase = MediaRelayBundlePhase::Idle;
  bool ack_ok = false;
};

MediaRelayAttachAckDecision DecideMediaRelayAttachAck(const MediaRelayAttachAckContext& ctx);

enum class MediaRelayBundleCloseDecision {
  Ignore = 0,
  FailSession,
  SuppressNotify,
};

struct MediaRelayBundleCloseContext {
  MediaRelayBundlePhase phase = MediaRelayBundlePhase::Idle;
  bool local_cancel = false;
  bool remote_terminal = false;
  bool finished = false;
};

MediaRelayBundleCloseDecision DecideMediaRelayBundleClose(const MediaRelayBundleCloseContext& ctx);

bool MediaRelayBundlePhaseIsActive(MediaRelayBundlePhase phase);

/** Default volunteer quote (parity with MediaRelayRuntime::BuildQuote defaults). */
MediaRelayQuote BuildDefaultMediaRelayQuote(const MediaRelayQuoteRequest& req, double rate = 0.0,
                                            const std::string& mode = "volunteer");

} // namespace pbr
