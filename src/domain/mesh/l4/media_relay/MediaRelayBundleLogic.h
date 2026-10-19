#pragma once

#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
#include "common/directory/RelayScope.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
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

/**
 * Host: quotes issued and not yet accepted. The accept may arrive on another channel (a direct
 * quote channel is one-shot), so a quote cannot die with its channel — it expires after `ttl`
 * instead, and the book is capped. Without this a quote nobody accepted stayed until the service
 * stopped (V050 invitees quote hops while ringing and never accept those quotes).
 */
class MediaRelayQuoteBook {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::seconds kDefaultTtl{60};
  static constexpr size_t kDefaultCapacity = 4096;
  /** Pending quotes one requester may hold, so a single peer cannot fill the book for everyone. */
  static constexpr size_t kDefaultPerPeerCapacity = 16;

  struct Entry {
    MediaRelayQuote quote;
    std::string call_id;
    std::string requester;
    Clock::time_point issued_at;
  };

  explicit MediaRelayQuoteBook(std::chrono::milliseconds ttl = kDefaultTtl, size_t capacity = kDefaultCapacity,
                               size_t per_peer_capacity = kDefaultPerPeerCapacity)
      : ttl_(ttl), capacity_(capacity), per_peer_capacity_(per_peer_capacity) {}

  /**
   * Record a quote issued to `requester` (expired entries are dropped first). False when the book,
   * or that requester's share of it, is still full: refuse the quote.
   */
  bool Add(const MediaRelayQuote& quote, const std::string& call_id, const std::string& requester,
           Clock::time_point now);
  /** The live quote for an accept, left in place (admit first, then Take); null when unknown or expired. */
  const Entry* Find(const std::string& quote_id, Clock::time_point now) const;
  /** Remove and return the quote for an accept; nullopt when unknown or expired. */
  std::optional<Entry> Take(const std::string& quote_id, Clock::time_point now);
  /** Drop expired quotes (host tick). */
  void Expire(Clock::time_point now);
  void Clear() {
    entries_.clear();
    per_requester_.clear();
  }
  size_t size() const { return entries_.size(); }

private:
  using Entries = std::unordered_map<std::string, Entry>;

  bool Expired(const Entry& entry, Clock::time_point now) const { return now - entry.issued_at >= ttl_; }
  Entries::iterator Erase(Entries::iterator it);

  std::chrono::milliseconds ttl_;
  size_t capacity_;
  size_t per_peer_capacity_;
  Entries entries_;
  std::unordered_map<std::string, size_t> per_requester_;
};

} // namespace pbr
