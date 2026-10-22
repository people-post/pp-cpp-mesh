#include "mesh/dht/client/DhtClient.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/PeerId.h"
#include "amp/link/PeerLink.h"
#include "mesh/dht/DhtKey.h"
#include "mesh/dht/DhtRecordCodec.h"
#include "common/Utilities.h"
#include "crypto/SodiumUtil.h"
#include "mesh/shared/AmpChannelOpen.h"

#include <algorithm>
#include <ctime>
#include <optional>
namespace pp::mesh {
namespace {

using Clock = std::chrono::steady_clock;


std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
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

DhtClient::DhtClient(pp::amp::MeshRuntime& runtime, DhtRecordStore& store, DhtRoutingTable& table)
    : runtime_(runtime), store_(store), table_(table) {}

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

void DhtClient::NoteHeard(const std::string& peer_id, const std::string& dial_addr) {
  if (peer_id.empty() || dial_addr.empty()) {
    return;
  }
  const int64_t now_ms = NowMs();
  const DhtRoutingTable::HeardResult heard = table_.NoteHeard(peer_id, dial_addr, now_ms);
  if (heard.heard == DhtRoutingTable::Heard::Pending && heard.ping &&
      now_ms - heard.ping->last_heard_ms >= kQuietBeforePingMs) {
    PingContact(*heard.ping);
  }
}

void DhtClient::NoteAnswered(const std::string& peer_key, const common::Object& response,
                             const std::optional<PeerRoutingRecord>& its_record, const std::string& reached_at) {
  if (!response.getIf<bool>("participant").value_or(false)) {
    return;
  }
  const pp::amp::PeerLink* link = runtime_.Links().FindLink(peer_key);
  if (!link) {
    return;
  }
  const std::string peer_id = link->RemotePeerId();
  if (its_record && its_record->peer_id == peer_id) {
    NoteHeard(peer_id, PeerRoutingRecordDialAddr(*its_record));
  } else if (auto stored = store_.Get(peer_id)) {
    NoteHeard(peer_id, PeerRoutingRecordDialAddr(*stored));
  } else if (!reached_at.empty() && pp::amp::PeerIdFromMultiaddr(reached_at) == peer_id) {
    NoteHeard(peer_id, reached_at);
  }  // else admitted once its record is cached (the publish tick warms the query peers')
}

void DhtClient::PingContact(const DhtContact& contact) {
  if (HeldDown(contact.peer_id)) {
    table_.Remove(contact.peer_id);  // the record already knows: no ping needed
    return;
  }
  {
    std::lock_guard lock(ping_mutex_);
    if (!pinging_.insert(contact.peer_id).second) {
      return;
    }
  }
  if (!runtime_.Links().GetLinkSnapshot(contact.peer_id).has_endpoint) {
    (void)runtime_.Links().RegisterEndpoint(contact.peer_id, contact.multiaddr);
  }
  common::Object request;
  request.set("op", "ping");
  request.set("req_id", util::GenerateUuid());
  request.set("version", int64_t{kDhtWireVersion});
  Rpc(contact.peer_id, request, [this, contact](RpcRoe resp) {
    {
      std::lock_guard lock(ping_mutex_);
      pinging_.erase(contact.peer_id);
    }
    if (resp && resp->getString("op").value_or("") == "pong" &&
        resp->getString("peer_id").value_or("") == contact.peer_id) {
      table_.NoteHeard(contact.peer_id, contact.multiaddr, NowMs());
    } else if (resp || resp.error().GetCode() != Err::NotStarted) {
      table_.Remove(contact.peer_id);  // a replacement moves in
    }
  });
}

void DhtClient::Rpc(const std::string& peer_key, common::Object request, std::function<void(RpcRoe)> on_response) {
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
  const std::string request_json = common::DumpJson(request);
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
                                         common::TryParseObject(std::string(frame->begin(), frame->end()));
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

std::vector<std::string> DhtClient::PublishableAddrs() const {
  std::vector<std::string> addrs =
      config_.current_listen_multiaddrs ? config_.current_listen_multiaddrs() : config_.listen_multiaddrs;
  // Only addresses others can dial us at, under our own PeerId.
  std::erase_if(addrs, [&](const std::string& ma) {
    return ma.find("/adp/") == std::string::npos || pp::amp::PeerIdFromMultiaddr(ma) != config_.local_peer_id;
  });
  return addrs;
}

void DhtClient::PublishSelfIfDue() {
  if (!IsStarted() || !config_.participate || config_.local_peer_id.empty()) {
    return;
  }
  const auto now = Clock::now();
  if (now < next_gate_check_) {
    return;
  }
  next_gate_check_ = now + std::chrono::milliseconds(kGateCheckMs);
  WarmQueryPeersIfDue(now);
  if (retry_publish_.exchange(false, std::memory_order_acq_rel)) {
    next_self_publish_ = std::min(next_self_publish_, now + std::chrono::seconds(kPublishRetrySeconds));
  }
  std::vector<std::string> addrs = PublishableAddrs();
  const bool moved = !published_addrs_.empty() && addrs != published_addrs_;  // a network change
  if (now < next_self_publish_ && !moved) {
    return;
  }
  // The gate (docs/DHT.md § 4.5): something dialable, and not Blocked. Closed: look at the next check.
  if (addrs.empty() || (config_.may_publish && !config_.may_publish())) {
    return;
  }
  if (publishing_.exchange(true, std::memory_order_acq_rel)) {
    return;  // the last one is still walking
  }
  const int ttl = config_.tunables.record_ttl_seconds > 0 ? config_.tunables.record_ttl_seconds : 3600;
  next_self_publish_ = now + std::chrono::seconds(std::max(30, ttl / 2));
  published_addrs_ = addrs;

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
    publishing_.store(false, std::memory_order_release);
    return;
  }
  if (!config_.device_signing_public.empty()) {
    signed_record->public_key_b64 = pp::Base64Encode(config_.device_signing_public);  // checkable by anyone
  }
  store_.Put(*signed_record);

  common::Object store_req;
  store_req.set("op", "store");
  store_req.set("req_id", util::GenerateUuid());
  store_req.set("version", int64_t{kDhtWireVersion});
  store_req.set("record", PeerRoutingRecordToObject(*signed_record));

  // At the k closest to our key that answered a walk to it, and at the query peers (where a
  // Client, which does not walk, asks).
  Walk(config_.local_peer_id, DhtLookup::Limits::WalkToClosest(table_.K()), [this, store_req](DhtLookup::Outcome outcome) {
    if (outcome.answered == 0) {
      retry_publish_.store(true, std::memory_order_release);  // nobody heard us: soon, not at ttl / 2
    }
    std::vector<std::string> targets = std::move(outcome.closest);
    for (const std::string& peer_key : config_.query_peer_keys) {
      if (std::find(targets.begin(), targets.end(), peer_key) == targets.end()) {
        targets.push_back(peer_key);
      }
    }
    for (const std::string& peer_key : targets) {
      if (peer_key != config_.local_peer_id && !HeldDown(peer_key)) {
        Rpc(peer_key, store_req, [](RpcRoe) {});
      }
    }
    {
      std::lock_guard lock(stats_mutex_);
      ++self_publishes_;
      last_publish_targets_ = targets.size();
    }
    publishing_.store(false, std::memory_order_release);
  });

}

void DhtClient::WarmQueryPeersIfDue(const std::chrono::steady_clock::time_point now) {
  if (now < next_warm_) {
    return;
  }
  next_warm_ = now + std::chrono::seconds(kWarmSeconds);
  // The query peers' own records in the local store: their addresses for the table, dial
  // warm-up. Looked for again until found — a startup miss must not wait for the next publish.
  const int64_t now_sec = static_cast<int64_t>(std::time(nullptr));
  for (const std::string& peer_key : FilteredQueryPeerKeys()) {
    if (peer_key.empty() || peer_key == config_.local_peer_id || HeldDown(peer_key)) {
      continue;
    }
    if (auto hit = store_.Get(peer_key); hit && !PeerRoutingRecordExpired(*hit, now_sec)) {
      continue;
    }
    FindPeer(peer_key, [](FindPeerRoe) {});
  }
}

bool DhtClient::HeldDown(const std::string& peer_id) const {
  return config_.reach_held_down && config_.reach_held_down(peer_id) && !runtime_.Links().IsConnected(peer_id);
}

void DhtClient::SweepIfDue() {
  const auto now = Clock::now();
  if (!IsStarted() || !config_.reach_held_down || now < next_sweep_) {
    return;
  }
  next_sweep_ = now + std::chrono::seconds(kSweepSeconds);
  SweepHeldDown();
}

void DhtClient::SweepHeldDown() {
  if (!config_.reach_held_down) {
    return;
  }
  // Entries the reach record holds down leave the table (a replacement moves in) — D5.
  for (const DhtContact& contact : table_.Closest(table_.SelfKey(), table_.Size())) {
    if (HeldDown(contact.peer_id)) {
      table_.Remove(contact.peer_id);
    }
  }
}

void DhtClient::RefreshIfDue() {
  if (!IsStarted() || !config_.participate) {
    return;
  }
  const auto now = Clock::now();
  if (now < next_refresh_check_) {
    return;
  }
  next_refresh_check_ = now + std::chrono::seconds(kRefreshCheckSeconds);
  if (now >= refresh_window_end_) {
    refresh_window_end_ = now + std::chrono::hours(1);
    refreshes_in_window_ = 0;
  }
  // A bucket nobody in it was heard from for an hour: walk to one of its contacts — its key is
  // in the bucket's range, so the walk meets that part of the space (and checks the contact).
  const int64_t now_ms = NowMs();
  for (const int bucket : table_.StaleBuckets(now_ms - kStaleBucketMs)) {
    if (refreshes_in_window_ >= kMaxRefreshesPerHour) {
      return;
    }
    const auto in_bucket = table_.InBucket(bucket);
    if (in_bucket.empty()) {
      continue;
    }
    ++refreshes_in_window_;
    Walk(in_bucket[static_cast<size_t>(now_ms) % in_bucket.size()].peer_id, DhtLookup::Limits::WalkToClosest(table_.K()),
         [](DhtLookup::Outcome) {});
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

  const int64_t now_ms = NowMs();
  {
    std::lock_guard lock(not_found_mutex_);
    std::erase_if(not_found_until_ms_, [&](const auto& entry) { return entry.second <= now_ms; });
    if (not_found_until_ms_.contains(target_peer_id)) {
      inflight_lookups_.fetch_sub(1, std::memory_order_acq_rel);
      on_done(FindPeerRoe::error(Failure::Of(Err::NotFound, "find_peer: not found lately")));
      return;
    }
  }

  {
    std::lock_guard lock(stats_mutex_);
    ++find_peer_issued_;
  }
  Walk(target_peer_id, DhtLookup::Limits::ForK(table_.K()), [this, target_peer_id, on_done](WalkOutcome outcome) {
    inflight_lookups_.fetch_sub(1, std::memory_order_acq_rel);
    if (outcome.record) {
      store_.Put(*outcome.record);
      DhtFindPeerResult result;
      result.peer_id = target_peer_id;
      result.record = std::move(*outcome.record);
      on_done(std::move(result));
      return;
    }
    if (outcome.answered > 0) {  // someone answered and nobody had it — not just our link down
      {
        std::lock_guard lock(not_found_mutex_);
        not_found_until_ms_[target_peer_id] = NowMs() + kNotFoundCacheMs;
      }
      on_done(FindPeerRoe::error(Failure::Of(Err::NotFound, "find_peer not found")));
      return;
    }
    on_done(FindPeerRoe::error(outcome.asked == 0
                                   ? Failure::Of(Err::InvalidRequest, "no dht query peers configured")
                                   : outcome.last_failure));
  });
}

void DhtClient::Walk(const std::string& target_peer_id, DhtLookup::Limits limits,
                     std::function<void(WalkOutcome)> done) {
  // Seeds: the query peers, and (taking part) the routing table's closest to the target.
  std::vector<DhtLookupCandidate> seeds;
  for (const std::string& peer_key : FilteredQueryPeerKeys()) {
    if (!HeldDown(peer_key)) {
      seeds.push_back({peer_key, std::nullopt, {}});
    }
  }
  if (config_.participate) {
    if (const auto target_key = DhtKeyOfPeerId(target_peer_id)) {
      for (DhtContact& contact : table_.Closest(*target_key, table_.K())) {
        if (SoftReputationAllows(contact.peer_id) && !HeldDown(contact.peer_id)) {
          seeds.push_back({std::move(contact.peer_id), contact.key, std::move(contact.multiaddr)});
        }
      }
    }
  }
  auto last_failure = std::make_shared<std::pair<std::mutex, Failure>>();
  last_failure->second = Failure::Of(Err::NotFound, "find_peer not found");
  limits.follow_hints = limits.follow_hints && config_.participate;
  limits.budget = std::chrono::milliseconds(kLookupBudgetMs);
  DhtLookup::Run(
      target_peer_id, std::move(seeds), limits,
      [this, target_peer_id, last_failure](const DhtLookupCandidate& candidate, DhtLookup::AnswerCb answer) {
        AskForPeer(candidate, target_peer_id, [last_failure, answer](CodedRoe<DhtLookupAnswer, Err> asked) {
          if (asked) {
            answer(std::move(*asked));
            return;
          }
          {
            std::lock_guard lock(last_failure->first);
            last_failure->second = asked.error();
          }
          answer(std::nullopt);
        });
      },
      [this](const std::string& peer_id) {
        return peer_id == config_.local_peer_id || !SoftReputationAllows(peer_id) || HeldDown(peer_id);
      },
      [last_failure, done = std::move(done)](DhtLookup::Outcome outcome) {
        WalkOutcome out;
        static_cast<DhtLookup::Outcome&>(out) = std::move(outcome);
        {
          std::lock_guard lock(last_failure->first);
          out.last_failure = last_failure->second;
        }
        done(std::move(out));
      });
}

void DhtClient::AskForPeer(const DhtLookupCandidate& candidate, const std::string& target_peer_id,
                           std::function<void(CodedRoe<DhtLookupAnswer, Err>)> done) {
  const std::string& peer_key = candidate.peer_key;
  if (!candidate.hint_addr.empty() && !runtime_.Links().GetLinkSnapshot(peer_key).has_endpoint) {
    (void)runtime_.Links().RegisterEndpoint(peer_key, candidate.hint_addr);  // dialling it proves the PeerId
  }
  common::Object request;
  request.set("op", "find_peer");
  request.set("req_id", util::GenerateUuid());
  request.set("version", int64_t{kDhtWireVersion});
  request.set("peer_id", target_peer_id);
  Rpc(peer_key, request, [this, peer_key, target_peer_id, reached_at = candidate.hint_addr,
                           done = std::move(done)](RpcRoe resp) {
    if (!resp) {
      done(CodedRoe<DhtLookupAnswer, Err>::error(resp.error()));
      return;
    }
    if (resp->getString("op").value_or("") != "find_peer_result") {
      done(CodedRoe<DhtLookupAnswer, Err>::error(Failure::Of(
          Err::ProtocolError, "find_peer: " + resp->getString("code").value_or("unexpected answer"))));
      return;
    }
    bool saw_bad = false;
    DhtLookupAnswer answer;
    std::optional<PeerRoutingRecord> self_answer;  // the responder's own, verified
    const pp::amp::PeerLink* answering_link = runtime_.Links().FindLink(peer_key);
    const std::string responder = answering_link ? answering_link->RemotePeerId() : std::string();
    const int64_t now_sec = static_cast<int64_t>(std::time(nullptr));
    if (const common::Array* records = resp->getArray("records")) {
      for (const common::Value& item : records->elements) {
        const common::Object* obj = common::asObject(item);
        auto parsed = obj ? PeerRoutingRecordFromObject(*obj) : Roe<PeerRoutingRecord>(Error("not an object"));
        if (!parsed || parsed->peer_id != target_peer_id) {
          saw_bad = true;
          continue;
        }
        if (PeerRoutingRecordExpired(*parsed, now_sec)) {
          continue;  // stale, or our clocks differ — not misbehaviour
        }
        bool from_subject = false;
        if (!responder.empty() && responder == target_peer_id) {
          // Its subject answered: the session proved the key; the signature must hold.
          const pp::amp::ByteVector& key = answering_link->RemoteIdentityPublicKey();
          auto verified = VerifyPeerRoutingRecord(*parsed, key);
          if (!verified || !*verified) {
            saw_bad = true;
            continue;
          }
          parsed->public_key_b64 = pp::Base64Encode(key);
          from_subject = true;
          self_answer = *parsed;
        } else if (parsed->public_key_b64.empty()) {
          continue;  // an older publisher's record: not checkable from here — dropped
        } else if (!VerifyPeerRoutingRecordStandalone(*parsed)) {
          saw_bad = true;  // carries a key, and does not check out
          continue;
        }
        // A self answer always wins over another's, whatever the seq: a third party cannot
        // suppress a legitimate self answer by racing it with a higher seq.
        if (!answer.record || (from_subject && !answer.self_answer) ||
            (from_subject == answer.self_answer && parsed->seq > answer.record->seq)) {
          answer.record = std::move(*parsed);
          answer.self_answer = from_subject;
        }
      }
    }
    answer.closer = DhtCloserPeersFromResponse(*resp);
    if (saw_bad) {
      NoteSoftReputationBad(peer_key);
    } else {
      NoteAnswered(peer_key, *resp, self_answer, reached_at);
    }
    done(std::move(answer));
  });
}

DhtClient::Stats DhtClient::OutboundStats() const {
  Stats stats;
  {
    std::lock_guard lock(stats_mutex_);
    stats.find_peer_issued = find_peer_issued_;
    stats.soft_reputation_skips = soft_reputation_skips_;
    stats.self_publishes = self_publishes_;
    stats.last_publish_targets = last_publish_targets_;
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

} // namespace pp::mesh
