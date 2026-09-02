#pragma once

#include "foundation/data/Config.h"
#include "common/directory/RelayScope.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

inline constexpr const char* kMediaRelayProtocolId = "/pp-browser/media-relay/1.0.0";

/** V032 host load limits (also documented in HOST_RECEIVE_POLICY). */
inline constexpr size_t kMediaRelayMaxHostSessions = 4;
inline constexpr size_t kMediaRelayMaxParticipantsPerSession = 8;

/** Per-inbound-stream control/attach phases (N026 / MEDIA_RELAY_ATTACH.md). */
enum class MediaRelayAttachPhase {
  Control = 0,
  Quoted,
  Accepted,
  Attaching,
  Attached,
  Rejected,
  Closed,
};

enum class MediaRelayAttachEvent {
  StreamOpened = 0,
  OpQuote,
  OpAccept,
  OpAttach,
  OpUnsupported,
  AdmitFail,
  AttachOk,
  AttachFail,
  Cancel,
};

const char* MediaRelayAttachPhaseName(MediaRelayAttachPhase phase);
const char* MediaRelayAttachEventName(MediaRelayAttachEvent ev);

/** Client outbound attach phases (phone→hop AcceptAndAttach) — N026. */
enum class MediaRelayClientPhase {
  Idle = 0,
  Dialing,
  Accepting,
  Attaching,
  Attached,
  Detaching,
};

enum class MediaRelayClientEvent {
  AttachRequested = 0,
  OpenStreamOk,
  OpenStreamFail,
  AcceptOk,
  AcceptFail,
  AttachOk,
  AttachFail,
  DetachRequested,
  AttachTimeout,
  DuplexLost,
  AttachSuperseded,
};

const char* MediaRelayClientPhaseName(MediaRelayClientPhase phase);
const char* MediaRelayClientEventName(MediaRelayClientEvent ev);

/** N021 channel_type QoS classes (not codecs). */
enum class MediaChannelType : uint8_t {
  ReliableOrdered = 0,
  LatestLossy = 1,
  BestEffort = 2,
};

struct MediaRelayQuoteRequest {
  std::string call_id;
  int participants = 1;
  int64_t want_up_bps = 0;
  int64_t want_down_bps = 0;
};

struct MediaRelayQuote {
  bool ok = false;
  std::string error;
  std::string quote_id;
  int64_t a_up_bps = 0;
  int64_t a_down_bps = 0;
  int64_t b_up_bps = 0;
  int64_t b_down_bps = 0;
  std::string pricing_mode = "volunteer";
  double rate = 0.0;
  int64_t ceiling_bytes = 0;
  double ceiling_amount = 0.0;
};

struct MediaRelayAttachResult {
  bool ok = false;
  std::string error;
  std::string session_token;
};

struct MediaRelayAdmissionPolicy {
  bool prefer_contacts_only = false;
  RelayScopeMask serve_scope_mask = kRelayScopeVolunteerServe;
  std::unordered_set<std::string> contact_peer_ids;
};

struct MediaDataFrame {
  uint32_t stream_id = 0;
  uint16_t channel_id = 0;
  MediaChannelType channel_type = MediaChannelType::BestEffort;
  uint32_t seq = 0;
  uint8_t mark = 0;
  std::vector<uint8_t> payload;
};

/** Encode/decode N021 data frames (body after u64-BE length prefix). */
std::vector<uint8_t> EncodeMediaDataFrame(const MediaDataFrame& frame);
Roe<MediaDataFrame> DecodeMediaDataFrame(const std::vector<uint8_t>& body);

} // namespace pbr
