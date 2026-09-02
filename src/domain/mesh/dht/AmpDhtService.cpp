#include "domain/mesh/dht/AmpDhtService.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "domain/mesh/dht/DhtRecordCodec.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const AmpDhtService::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

std::chrono::milliseconds ControlTimeout(const MeshDhtConfig& tunables) {
  const int ms = tunables.find_peer_timeout_ms > 0 ? tunables.find_peer_timeout_ms : 5000;
  return std::chrono::milliseconds(ms);
}

Object MakeErrorResponse(const std::string& req_id, const std::string& code, const std::string& message) {
  Object object;
  object.set("op", "error");
  object.set("req_id", req_id);
  object.set("version", int64_t{kDhtWireVersion});
  object.set("code", code);
  object.set("message", message);
  return object;
}

Value RecordsToJsonArray(const std::vector<PeerRoutingRecord>& records) {
  std::vector<Value> elements;
  elements.reserve(records.size());
  for (const PeerRoutingRecord& record : records) {
    elements.emplace_back(std::make_shared<Object>(PeerRoutingRecordToObject(record)));
  }
  return makeArray(std::move(elements));
}

} // namespace

AmpDhtService::Failure AmpDhtService::WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child) {
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

struct AmpDhtService::Impl {
  pp::amp::PeerLinkManager* links = nullptr;
  AmpDhtService* self = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  std::atomic<bool> stopped{false};

  void IoPumpUntil(const std::function<bool()>& done, const Clock::time_point deadline) {
    while (!done() && Clock::now() < deadline) {
      if (io_pump) {
        io_pump();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }

  void HandleInboundOnLink(pp::amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !links || !self) {
      return;
    }
    auto session = std::make_shared<pp::amp::ChannelSession>();
    session->Bind(*link.Mux(), channel_id, pp::amp::ControlJsonChannelPolicy(),
                  [this, session, &link](Roe<std::vector<uint8_t>> frame) {
                    if (!frame || stopped.load(std::memory_order_acquire)) {
                      return false;
                    }
                    auto body = std::move(*frame);
                    RunWorker(post_worker, [this, session, body = std::move(body), remote_pk = link.RemoteIdentityPublicKey(),
                                            remote_peer = link.RemotePeerId()]() mutable {
                      if (stopped.load(std::memory_order_acquire) || !self) {
                        return;
                      }
                      const std::string json_utf8(body.begin(), body.end());
                      auto root = TryParseObject(json_utf8);
                      if (!root) {
                        Object resp = MakeErrorResponse("", "invalid_json", "invalid json");
                        (void)session->EnqueueOutbound(JsonToBody(DumpJson(resp)));
                        session->Close();
                        return;
                      }
                      const std::string req_id = root->getString("req_id").value_or("");
                      const int version = static_cast<int>(root->getIf<int64_t>("version").value_or(0));
                      if (version != kDhtWireVersion) {
                        Object resp = MakeErrorResponse(req_id, "bad_version", "unsupported version");
                        (void)session->EnqueueOutbound(JsonToBody(DumpJson(resp)));
                        session->Close();
                        return;
                      }
                      const std::string op = root->getString("op").value_or("");
                      Object response;
                      if (op == "ping") {
                        response.set("op", "pong");
                        response.set("req_id", req_id);
                        response.set("version", int64_t{kDhtWireVersion});
                        response.set("peer_id", self->config_.local_peer_id);
                      } else if (op == "find_peer" || op == "store") {
                        if (!self->AllowInbound(remote_peer)) {
                          {
                            std::lock_guard lock(self->stats_mutex_);
                            ++self->inbound_rate_limited_;
                          }
                          response = MakeErrorResponse(req_id, "rate_limited", "dht rate limited");
                        } else if (op == "find_peer") {
                          {
                            std::lock_guard lock(self->stats_mutex_);
                            ++self->inbound_find_peer_;
                          }
                          response.set("op", "find_peer_result");
                          response.set("req_id", req_id);
                          response.set("version", int64_t{kDhtWireVersion});
                          const std::string peer_id = root->getString("peer_id").value_or("");
                          response.set("peer_id", peer_id);
                          std::vector<PeerRoutingRecord> records;
                          if (auto local = self->store_.Get(peer_id)) {
                            records.push_back(*local);
                          }
                          response.set("records", RecordsToJsonArray(records));
                          response.set("closer_peers", makeArray(std::vector<Value>{}));
                        } else {
                          {
                            std::lock_guard lock(self->stats_mutex_);
                            ++self->inbound_store_;
                          }
                          std::string reject_code;
                          bool ok = false;
                          if (const Object* record_obj = root->getObject("record")) {
                            if (auto parsed = PeerRoutingRecordFromObject(*record_obj)) {
                              if (parsed->peer_id != remote_peer) {
                                reject_code = "not_self";
                              } else if (PeerRoutingRecordExpired(
                                             *parsed, static_cast<int64_t>(std::time(nullptr)))) {
                                reject_code = "expired";
                              } else {
                                auto verified = VerifyPeerRoutingRecord(*parsed, remote_pk);
                                if (!verified) {
                                  reject_code = "bad_signature";
                                } else if (!*verified) {
                                  reject_code = "bad_signature";
                                } else if (auto existing = self->store_.Get(parsed->peer_id);
                                           existing && existing->seq > parsed->seq) {
                                  reject_code = "seq_regression";
                                } else if (!self->store_.Put(*parsed)) {
                                  reject_code = "store_failed";
                                } else {
                                  ok = true;
                                }
                              }
                            } else {
                              reject_code = "invalid_record";
                            }
                          } else {
                            reject_code = "missing_record";
                          }
                          if (!ok) {
                            std::lock_guard lock(self->stats_mutex_);
                            ++self->store_rejected_;
                          }
                          if (!ok && !reject_code.empty()) {
                            response = MakeErrorResponse(req_id, reject_code, reject_code);
                          } else {
                            response.set("op", "store_result");
                            response.set("req_id", req_id);
                            response.set("version", int64_t{kDhtWireVersion});
                            response.set("ok", ok);
                          }
                        }
                      } else {
                        response = MakeErrorResponse(req_id, "unsupported_op", "unsupported op");
                      }
                      (void)session->EnqueueOutbound(JsonToBody(DumpJson(response)));
                      if (io_pump) {
                        io_pump();
                      }
                      session->Close();
                    });
                    return false;
                  });
  }

  void Rpc(const std::string& peer_key, Object request, std::function<void(RpcRoe)> on_response) {
    if (stopped.load(std::memory_order_acquire) || !links) {
      on_response(RpcRoe::error(Failure::Of(Err::NotStarted, "dht service stopped")));
      return;
    }
    if (!links->GetLinkSnapshot(peer_key).has_endpoint) {
      on_response(RpcRoe::error(Failure::Of(Err::EndpointNotRegistered, "dht peer endpoint not registered")));
      return;
    }
    const auto timeout = ControlTimeout(self->config_.tunables);
    const auto deadline = Clock::now() + timeout + std::chrono::milliseconds(2000);
    const std::string request_json = DumpJson(request);
    auto session = std::make_shared<pp::amp::ChannelSession>();
    auto settled = std::make_shared<std::atomic<bool>>(false);

    auto finish = [settled, session, on_response = std::move(on_response)](RpcRoe value) {
      if (settled->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      session->Close();
      on_response(std::move(value));
    };

    links->EnsureAssociation(peer_key, [this, peer_key, request_json, finish, session, deadline,
                                        timeout](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
      if (!assoc) {
        finish(RpcRoe::error(WrapLinkFailure(assoc.error())));
        return;
      }
      links->OpenChannel(peer_key, kDhtProtocolId, pp::amp::ControlJsonChannelPolicy(timeout),
                         [this, peer_key, request_json, finish, session, deadline,
                          timeout](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
                           if (!channel) {
                             finish(RpcRoe::error(WrapLinkFailure(channel.error())));
                             return;
                           }
                           IoPumpUntil(
                               [&] {
                                 auto* link = links->FindLink(peer_key);
                                 return link && link->Mux() &&
                                        link->Mux()->State(*channel) == pp::amp::ChannelState::Open;
                               },
                               deadline);
                           auto* link = links->FindLink(peer_key);
                           if (!link || !link->Mux() ||
                               link->Mux()->State(*channel) != pp::amp::ChannelState::Open) {
                             finish(RpcRoe::error(Failure::Of(Err::ChannelFailed, "dht channel open failed")));
                             return;
                           }
                           session->Bind(*link->Mux(), *channel, pp::amp::ControlJsonChannelPolicy(timeout),
                                         [finish](Roe<std::vector<uint8_t>> frame) {
                                           if (!frame) {
                                             finish(RpcRoe::error(
                                                 Failure::Of(Err::ProtocolError, "dht response read failed")));
                                             return false;
                                           }
                                           auto root = TryParseObject(std::string(frame->begin(), frame->end()));
                                           if (!root) {
                                             finish(RpcRoe::error(
                                                 Failure::Of(Err::ProtocolError, "invalid dht response json")));
                                             return false;
                                           }
                                           finish(std::move(*root));
                                           return false;
                                         });
                           if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                             finish(RpcRoe::error(Failure::Of(Err::ProtocolError, "dht request send failed")));
                             return;
                           }
                           if (io_pump) {
                             io_pump();
                           }
                         });
    });
  }
};

AmpDhtService::AmpDhtService(pp::amp::PeerLinkManager& links, IoPump io_pump, WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
  impl_->self = this;
}

AmpDhtService::~AmpDhtService() { Stop(); }

void AmpDhtService::Configure(AmpDhtServiceConfig config) {
  config_ = std::move(config);
  inbound_limiter_.Configure(config_.tunables.inbound_ops_per_peer_per_window,
                             config_.tunables.inbound_rate_window_seconds);
}

bool AmpDhtService::AllowInbound(const std::string& remote_peer) {
  return inbound_limiter_.Allow(remote_peer);
}

void AmpDhtService::NoteSoftReputationBad(const std::string& peer_key) {
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

bool AmpDhtService::SoftReputationAllows(const std::string& peer_key) const {
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

std::vector<std::string> AmpDhtService::FilteredQueryPeerKeys() {
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

void AmpDhtService::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kDhtProtocolId, [impl = impl_.get()](pp::amp::PeerLink& link, const uint32_t channel_id) {
    impl->HandleInboundOnLink(link, channel_id);
  });
  if (config_.participate) {
    Tick();
  }
}

void AmpDhtService::Stop() {
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kDhtProtocolId);
}

void AmpDhtService::Tick() {
  if (!started_ || !config_.participate || config_.local_peer_id.empty()) {
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
    impl_->Rpc(peer_key, store_req, [](AmpDhtService::RpcRoe) {});
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

void AmpDhtService::FindPeer(const std::string& target_peer_id, std::function<void(FindPeerRoe)> on_done) {
  if (!started_) {
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
    std::optional<PeerRoutingRecord> best;
    Failure last_failure = Failure::Of(Err::NotFound, "find_peer not found");
    AmpDhtService* self = nullptr;
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
    impl_->Rpc(peer_key, request,
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
                           if (!state->best || parsed->seq > state->best->seq) {
                             state->best = std::move(*parsed);
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
                 if (state->best) {
                   store_.Put(*state->best);
                   DhtFindPeerResult result;
                   result.peer_id = target_peer_id;
                   result.record = *state->best;
                   finish_lookup(std::move(result));
                   return;
                 }
                 finish_lookup(FindPeerRoe::error(state->last_failure));
               });
  }
}

std::optional<PeerRoutingRecord> AmpDhtService::LocalRecord(const std::string& peer_id) const {
  return store_.Get(peer_id);
}

std::vector<PeerRoutingRecord> AmpDhtService::SnapshotRecords() const {
  return store_.Snapshot();
}

DhtOpsStats AmpDhtService::Stats() const {
  DhtOpsStats stats;
  stats.started = started_;
  stats.participate = config_.participate;
  stats.cached_records = store_.Size();
  {
    std::lock_guard lock(stats_mutex_);
    stats.inbound_find_peer = inbound_find_peer_;
    stats.inbound_store = inbound_store_;
    stats.inbound_rate_limited = inbound_rate_limited_;
    stats.store_rejected = store_rejected_;
    stats.find_peer_issued = find_peer_issued_;
    stats.soft_reputation_skips = soft_reputation_skips_;
  }
  {
    std::lock_guard lock(reputation_mutex_);
    const auto now = Clock::now();
    for (const auto& [peer, rep] : soft_reputation_) {
      (void)peer;
      if (now < rep.cooldown_until) {
        ++stats.soft_reputation_penalized_peers;
      }
    }
  }
  return stats;
}

std::string AmpDhtService::FormatOpsStatusJson() const {
  const DhtOpsStats stats = Stats();
  Object object;
  object.set("started", stats.started);
  object.set("participate", stats.participate);
  object.set("cached_records", static_cast<int64_t>(stats.cached_records));
  object.set("inbound_find_peer", static_cast<int64_t>(stats.inbound_find_peer));
  object.set("inbound_store", static_cast<int64_t>(stats.inbound_store));
  object.set("inbound_rate_limited", static_cast<int64_t>(stats.inbound_rate_limited));
  object.set("store_rejected", static_cast<int64_t>(stats.store_rejected));
  object.set("find_peer_issued", static_cast<int64_t>(stats.find_peer_issued));
  object.set("soft_reputation_skips", static_cast<int64_t>(stats.soft_reputation_skips));
  object.set("soft_reputation_penalized_peers",
             static_cast<int64_t>(stats.soft_reputation_penalized_peers));
  std::vector<Value> record_rows;
  for (const PeerRoutingRecord& record : SnapshotRecords()) {
    Object row;
    row.set("peer_id", record.peer_id);
    row.set("seq", record.seq);
    std::vector<Value> addrs;
    for (const std::string& ma : record.multiaddrs) {
      addrs.emplace_back(ma);
    }
    row.set("multiaddrs", makeArray(std::move(addrs)));
    if (record.capabilities) {
      Object caps;
      caps.set("circuit_relay", record.capabilities->circuit_relay);
      caps.set("media_relay", record.capabilities->media_relay);
      row.set("capabilities", std::move(caps));
    }
    record_rows.emplace_back(std::make_shared<Object>(std::move(row)));
  }
  object.set("records", makeArray(std::move(record_rows)));
  return DumpJson(object);
}

} // namespace pbr
