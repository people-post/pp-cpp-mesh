#include "domain/mesh/dht/client/DhtClient.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/PeerLink.h"
#include "domain/mesh/dht/DhtRecordCodec.h"
#include "common/Utilities.h"
#include "domain/mesh/shared/AmpChannelOpen.h"

#include <algorithm>
#include <ctime>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

// find_peer results carry a *third-party* record (the target's, not the responder's): we only
// know the responder's key, not the target's, so the signature cannot be verified here. Such a
// record is never persisted to the shared store (so it is never re-served/forwarded to other
// queriers) and its ttl_seconds is capped short. Note this only bounds the DHT store's own
// re-serve/cache window: the caller (ConversationsHub::ApplyDhtFindPeerResult) still registers
// the record's multiaddrs as dial endpoints regardless of ttl_seconds.
constexpr int64_t kUnverifiedFindPeerRecordTtlCapSeconds = 300;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

std::chrono::milliseconds ControlTimeout(const MeshDhtConfig& tunables) {
  const int ms = tunables.find_peer_timeout_ms > 0 ? tunables.find_peer_timeout_ms : 5000;
  return std::chrono::milliseconds(ms);
}

} // namespace

DhtClient::Failure DhtClient::WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child) {
  switch (child.GetCode()) {
    case pp::amp::PeerLinkManager::Err::EndpointNotRegistered:
      return Failure::Of(Err::EndpointNotRegistered,
                         detail::AppendFrom("dht: endpoint not registered", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialTimeout:
      return Failure::Of(Err::Timeout, detail::AppendFrom("dht: dial timed out", "link", child.message));
    case pp::amp::PeerLinkManager::Err::ChannelOpenFailed:
      return Failure::Of(Err::ChannelFailed,
                         detail::AppendFrom("dht: channel open failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialInBackoff:
    case pp::amp::PeerLinkManager::Err::TooManyConcurrentDials:
    case pp::amp::PeerLinkManager::Err::MaxLinksReached:
    case pp::amp::PeerLinkManager::Err::AssociationNotReady:
    case pp::amp::PeerLinkManager::Err::LinkNotFound:
    case pp::amp::PeerLinkManager::Err::NestedCarrierIncomplete:
    case pp::amp::PeerLinkManager::Err::HandshakeFailed:
    case pp::amp::PeerLinkManager::Err::TransportFailed:
    case pp::amp::PeerLinkManager::Err::DualDialLost:
      return Failure::Of(Err::LinkFailed, detail::AppendFrom("dht: link failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::Ok:
    case pp::amp::PeerLinkManager::Err::Generic:
    default:
      return Failure::Of(Err::Generic, detail::AppendFrom("dht: link error", "link", child.message));
  }
}

DhtClient::DhtClient(pp::amp::MeshRuntime& runtime, DhtRecordStore& store) : runtime_(runtime), store_(store) {}

void DhtClient::Configure(const AmpDhtProtocolConfig& config) { config_ = config; }

void DhtClient::Start() { started_.store(true, std::memory_order_release); }

void DhtClient::Stop() { started_.store(false, std::memory_order_release); }

void DhtClient::NoteSoftReputationBad(const std::string& peer_key) {
  if (peer_key.empty()) {
    return;
  }
  const int threshold = config_.tunables.soft_reputation_penalty_threshold > 0
                            ? config_.tunables.soft_reputation_penalty_threshold
                            : 3;
  const int cooldown_s = config_.tunables.soft_reputation_cooldown_seconds > 0
                             ? config_.tunables.soft_reputation_cooldown_seconds
                             : 300;
  std::lock_guard lock(reputation_mutex_);
  SoftRep& rep = soft_reputation_[peer_key];
  ++rep.bad_count;
  if (rep.bad_count >= threshold) {
    rep.cooldown_until = Clock::now() + std::chrono::seconds(cooldown_s);
    rep.bad_count = 0;
  }
}

bool DhtClient::SoftReputationAllows(const std::string& peer_key) const {
  if (peer_key.empty()) {
    return true;
  }
  std::lock_guard lock(reputation_mutex_);
  const auto it = soft_reputation_.find(peer_key);
  if (it == soft_reputation_.end()) {
    return true;
  }
  return Clock::now() >= it->second.cooldown_until;
}

std::vector<std::string> DhtClient::FilteredQueryPeerKeys() {
  std::vector<std::string> keys;
  keys.reserve(config_.query_peer_keys.size());
  for (const std::string& peer_key : config_.query_peer_keys) {
    if (SoftReputationAllows(peer_key)) {
      keys.push_back(peer_key);
    } else {
      std::lock_guard lock(stats_mutex_);
      ++soft_reputation_skips_;
    }
  }
  return keys;
}

void DhtClient::Rpc(const std::string& peer_key, Object request, std::function<void(RpcRoe)> on_response) {
  if (!IsStarted()) {
    on_response(RpcRoe::error(Failure::Of(Err::NotStarted, "dht service stopped")));
    return;
  }
  if (!runtime_.Links().GetLinkSnapshot(peer_key).has_endpoint) {
    on_response(RpcRoe::error(Failure::Of(Err::EndpointNotRegistered, "dht peer endpoint not registered")));
    return;
  }
  const auto timeout = ControlTimeout(config_.tunables);
  const auto deadline = Clock::now() + timeout + std::chrono::milliseconds(2000);
  const std::string request_json = DumpJson(request);
  auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
  auto settled = std::make_shared<std::atomic<bool>>(false);

  auto finish = [settled, session_holder, on_response = std::move(on_response)](RpcRoe value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (*session_holder) {
      (*session_holder)->Close();
    }
    on_response(std::move(value));
  };

  runtime_.Links().EnsureAssociation(peer_key, [this, peer_key, request_json, finish, session_holder, deadline,
                                      timeout](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
    if (!assoc) {
      finish(RpcRoe::error(WrapLinkFailure(assoc.error())));
      return;
    }
    runtime_.Links().OpenChannel(peer_key, kDhtProtocolId, pp::amp::ControlJsonChannelPolicy(timeout),
                       [this, peer_key, request_json, finish, session_holder, deadline,
                        timeout](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
                         if (!channel) {
                           finish(RpcRoe::error(WrapLinkFailure(channel.error())));
                           return;
                         }
                         if (!IsStarted()) {
                           finish(RpcRoe::error(Failure::Of(Err::NotStarted, "dht service stopped")));
                           return;
                         }
                         const uint32_t channel_id = *channel;
                         AmpWhenChannelOpen(
                             runtime_.Links(), peer_key, channel_id, deadline,
                             [this, peer_key, channel_id, request_json, finish, session_holder, timeout](
                                 bool open) mutable {
                               if (!IsStarted()) {
                                 finish(RpcRoe::error(Failure::Of(Err::NotStarted, "dht service stopped")));
                                 return;
                               }
                               if (!open) {
                                 finish(RpcRoe::error(
                                     Failure::Of(Err::ChannelFailed, "dht channel open failed")));
                                 return;
                               }
                               *session_holder = runtime_.Links().BindChannel(
                                   peer_key, channel_id, pp::amp::ControlJsonChannelPolicy(timeout),
                                   [finish](Roe<std::vector<uint8_t>> frame) {
                                     if (!frame) {
                                       finish(RpcRoe::error(Failure::Of(
                                           Err::ProtocolError, "dht response read failed")));
                                       return false;
                                     }
                                     auto root =
                                         TryParseObject(std::string(frame->begin(), frame->end()));
                                     if (!root) {
                                       finish(RpcRoe::error(Failure::Of(
                                           Err::ProtocolError, "invalid dht response json")));
                                       return false;
                                     }
                                     finish(std::move(*root));
                                     return false;
                                   },
                                   // The reply channel can end with no frame (read timeout, link
                                   // dropped, server stopping): settle, or the request never does.
                                   [finish](const char* reason) {
                                     finish(RpcRoe::error(Failure::Of(
                                         Err::ChannelFailed, std::string("dht channel closed: ") +
                                                                 (reason ? reason : ""))));
                                   });
                               if (!*session_holder) {
                                 finish(RpcRoe::error(
                                     Failure::Of(Err::ChannelFailed, "dht channel open failed")));
                                 return;
                               }
                               if (!(*session_holder)->EnqueueOutbound(JsonToBody(request_json))) {
                                 finish(RpcRoe::error(
                                     Failure::Of(Err::ProtocolError, "dht request send failed")));
                                 return;
                               }
                             });
                       });
  });
}

void DhtClient::PublishSelfIfDue() {
  if (!IsStarted() || !config_.participate || config_.local_peer_id.empty()) {
    return;
  }
  const auto now = Clock::now();
  if (now < next_self_publish_) {
    return;
  }
  const int ttl = config_.tunables.record_ttl_seconds > 0 ? config_.tunables.record_ttl_seconds : 3600;
  next_self_publish_ = now + std::chrono::seconds(std::max(30, ttl / 2));

  std::vector<std::string> addrs = config_.listen_multiaddrs;
  if (addrs.empty()) {
    return;
  }
  PeerRoutingRecord record;
  record.peer_id = config_.local_peer_id;
  record.seq = ++self_seq_;
  record.ttl_seconds = ttl;
  record.issued_at = static_cast<int64_t>(std::time(nullptr));
  record.multiaddrs = std::move(addrs);
  if (config_.publish_circuit_relay || config_.publish_media_relay) {
    PeerRoutingCapabilities caps;
    caps.circuit_relay = config_.publish_circuit_relay;
    caps.media_relay = config_.publish_media_relay;
    record.capabilities = caps;
  }
  auto signed_record = SignPeerRoutingRecord(record, config_.device_signing_secret);
  if (!signed_record) {
    return;
  }
  store_.Put(*signed_record);

  if (config_.query_peer_keys.empty()) {
    return;
  }
  Object store_req;
  store_req.set("op", "store");
  store_req.set("req_id", util::GenerateUuid());
  store_req.set("version", int64_t{kDhtWireVersion});
  store_req.set("record", PeerRoutingRecordToObject(*signed_record));
  for (const std::string& peer_key : config_.query_peer_keys) {
    Rpc(peer_key, store_req, [](RpcRoe) {});
  }

  // Warm bootstrap/query peers into the local store (lab mutual discovery + dial warm-up).
  const int64_t now_sec = static_cast<int64_t>(std::time(nullptr));
  for (const std::string& peer_key : FilteredQueryPeerKeys()) {
    if (peer_key.empty() || peer_key == config_.local_peer_id) {
      continue;
    }
    if (auto hit = store_.Get(peer_key); hit && !PeerRoutingRecordExpired(*hit, now_sec)) {
      continue;
    }
    FindPeer(peer_key, [](FindPeerRoe) {});
  }
}

void DhtClient::FindPeer(const std::string& target_peer_id, std::function<void(FindPeerRoe)> on_done) {
  if (!IsStarted()) {
    on_done(FindPeerRoe::error(Failure::Of(Err::NotStarted, "dht service not started")));
    return;
  }
  if (target_peer_id.empty()) {
    on_done(FindPeerRoe::error(Failure::Of(Err::InvalidRequest, "missing target peer_id")));
    return;
  }
  if (auto local = store_.Get(target_peer_id)) {
    DhtFindPeerResult result;
    result.peer_id = target_peer_id;
    result.record = *local;
    result.from_cache = true;
    on_done(std::move(result));
    return;
  }

  const int max_inflight =
      config_.tunables.max_concurrent_lookups > 0 ? config_.tunables.max_concurrent_lookups : 4;
  int expected = inflight_lookups_.load(std::memory_order_relaxed);
  while (true) {
    if (expected >= max_inflight) {
      on_done(FindPeerRoe::error(Failure::Of(Err::ConcurrencyLimit, "dht find_peer concurrency limit")));
      return;
    }
    if (inflight_lookups_.compare_exchange_weak(expected, expected + 1, std::memory_order_acq_rel)) {
      break;
    }
  }

  const std::vector<std::string> query_keys = FilteredQueryPeerKeys();
  if (query_keys.empty()) {
    inflight_lookups_.fetch_sub(1, std::memory_order_acq_rel);
    on_done(FindPeerRoe::error(Failure::Of(Err::InvalidRequest, "no dht query peers configured")));
    return;
  }

  {
    std::lock_guard lock(stats_mutex_);
    ++find_peer_issued_;
  }

  struct State {
    std::mutex mutex;
    std::shared_ptr<std::atomic<size_t>> pending;
    /** Highest-seq record where the answering peer's authenticated link identity is the
     * record's own peer_id (self-answer, verifiable trust). Tracked separately from
     * best_third_party so a third party can't suppress a legitimate self-answer by racing it
     * with a higher (forged) seq. */
    std::optional<PeerRoutingRecord> best_self;
    /** Highest-seq record from anyone else about target_peer_id — never verifiable here. */
    std::optional<PeerRoutingRecord> best_third_party;
    Failure last_failure = Failure::Of(Err::NotFound, "find_peer not found");
    DhtClient* self = nullptr;
  };
  auto state = std::make_shared<State>();
  state->self = this;
  state->pending = std::make_shared<std::atomic<size_t>>(query_keys.size());

  Object request;
  request.set("op", "find_peer");
  request.set("req_id", util::GenerateUuid());
  request.set("version", int64_t{kDhtWireVersion});
  request.set("peer_id", target_peer_id);

  auto finish_lookup = [state, on_done](FindPeerRoe value) {
    if (state->self) {
      state->self->inflight_lookups_.fetch_sub(1, std::memory_order_acq_rel);
    }
    on_done(std::move(value));
  };

  for (const std::string& peer_key : query_keys) {
    Rpc(peer_key, request,
               [this, state, target_peer_id, peer_key, finish_lookup](RpcRoe resp) mutable {
                 bool saw_bad = false;
                 {
                   std::lock_guard lock(state->mutex);
                   if (resp) {
                     if (const Array* records = resp->getArray("records")) {
                       for (const Value& item : records->elements) {
                         const Object* obj = asObject(item);
                         if (!obj) {
                           saw_bad = true;
                           continue;
                         }
                         if (auto parsed = PeerRoutingRecordFromObject(*obj)) {
                           if (parsed->peer_id != target_peer_id) {
                             saw_bad = true;
                             continue;
                           }
                           if (PeerRoutingRecordExpired(*parsed,
                                                       static_cast<int64_t>(std::time(nullptr)))) {
                             saw_bad = true;
                             continue;
                           }
                           const pp::amp::PeerLink* answering_link = runtime_.Links().FindLink(peer_key);
                           const bool is_self_answer =
                               answering_link != nullptr && answering_link->RemotePeerId() == target_peer_id;
                           if (is_self_answer) {
                             if (!state->best_self || parsed->seq > state->best_self->seq) {
                               state->best_self = std::move(*parsed);
                             }
                           } else {
                             if (!state->best_third_party || parsed->seq > state->best_third_party->seq) {
                               state->best_third_party = std::move(*parsed);
                             }
                           }
                         } else {
                           saw_bad = true;
                         }
                       }
                     }
                   } else {
                     state->last_failure = resp.error();
                   }
                 }
                 if (saw_bad) {
                   NoteSoftReputationBad(peer_key);
                 }
                 if (state->pending->fetch_sub(1, std::memory_order_acq_rel) != 1) {
                   return;
                 }
                 std::lock_guard lock(state->mutex);
                 // A verified self-answer always wins, regardless of any third-party seq: a
                 // single malicious query peer could otherwise suppress a legitimate self-answer
                 // by racing it with a higher forged seq for the same target.
                 if (state->best_self) {
                   store_.Put(*state->best_self);
                   DhtFindPeerResult result;
                   result.peer_id = target_peer_id;
                   result.record = *state->best_self;
                   finish_lookup(std::move(result));
                   return;
                 }
                 if (state->best_third_party) {
                   state->best_third_party->ttl_seconds =
                       std::min(state->best_third_party->ttl_seconds, kUnverifiedFindPeerRecordTtlCapSeconds);
                   DhtFindPeerResult result;
                   result.peer_id = target_peer_id;
                   result.record = *state->best_third_party;
                   finish_lookup(std::move(result));
                   return;
                 }
                 finish_lookup(FindPeerRoe::error(state->last_failure));
               });
  }
}

DhtClient::Stats DhtClient::OutboundStats() const {
  Stats stats;
  {
    std::lock_guard lock(stats_mutex_);
    stats.find_peer_issued = find_peer_issued_;
    stats.soft_reputation_skips = soft_reputation_skips_;
  }
  std::lock_guard lock(reputation_mutex_);
  const auto now = Clock::now();
  for (const auto& [peer, rep] : soft_reputation_) {
    (void)peer;
    if (now < rep.cooldown_until) {
      ++stats.soft_reputation_penalized_peers;
    }
  }
  return stats;
}

} // namespace pbr
