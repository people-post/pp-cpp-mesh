#include "domain/mesh/dht/serve/DhtServer.h"

#include "domain/mesh/l4/shared/InboundReply.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "domain/mesh/dht/DhtRecordCodec.h"
#include "common/ValueJson.h"
#include "foundation/runtime/DeferredSelf.h"

#include <atomic>
#include <ctime>
#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const DhtServer::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
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

struct DhtServer::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  DhtRecordStore* store = nullptr;
  WorkerPost post_worker;
  std::atomic<bool> stopped{false};
  /** Guards protocol-handler raw Impl* past Stop — OWNERSHIP.md § DeferredSelf. */
  DeferredSelf deferred;
  std::string local_peer_id;
  DhtRateLimiter limiter;
  mutable std::mutex stats_mutex;
  Stats stats;

  pp::amp::PeerLinkManager& Links() { return runtime->Links(); }
  /** IO lane for InboundReply. */
  InboundReply::IoPost IoPost() {
    return [rt = runtime](std::function<void()> task) { rt->PostToIo(std::move(task)); };
  }

  void HandleInboundOnLink(pp::amp::LinkHandle handle, const std::string& remote_peer_id,
                           const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime || remote_peer_id.empty()) {
      return;
    }
    pp::amp::ByteVector remote_pk;
    Links().WithLiveLink(handle, [&](pp::amp::PeerLink& link) {
      remote_pk = link.RemoteIdentityPublicKey();
    });
    auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    *session_holder = Links().BindChannel(
        remote_peer_id, channel_id, InboundReplyPolicy(pp::amp::ControlJsonChannelPolicy()),
        [this, session_holder, remote_pk = std::move(remote_pk),
         remote_peer = remote_peer_id](Roe<std::vector<uint8_t>> frame) mutable {
          auto session = *session_holder;
          if (!session || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          auto body = std::move(*frame);
          // Keep the channel open for the worker's reply (InboundReply.h); `reply` closes it.
          auto reply = MakeInboundReply(session, IoPost());
          RunWorker(post_worker, [this, reply, body = std::move(body), remote_pk = std::move(remote_pk),
                                  remote_peer = std::move(remote_peer)]() mutable {
            if (stopped.load(std::memory_order_acquire)) {
              return;
            }
            const std::string json_utf8(body.begin(), body.end());
            auto root = TryParseObject(json_utf8);
            if (!root) {
              Object resp = MakeErrorResponse("", "invalid_json", "invalid json");
              reply->Send(JsonToBody(DumpJson(resp)));
              reply->Close();
              return;
            }
            const std::string req_id = root->getString("req_id").value_or("");
            const int version = static_cast<int>(root->getIf<int64_t>("version").value_or(0));
            if (version != kDhtWireVersion) {
              Object resp = MakeErrorResponse(req_id, "bad_version", "unsupported version");
              reply->Send(JsonToBody(DumpJson(resp)));
              reply->Close();
              return;
            }
            const std::string op = root->getString("op").value_or("");
            Object response;
            if (op == "ping") {
              response.set("op", "pong");
              response.set("req_id", req_id);
              response.set("version", int64_t{kDhtWireVersion});
              response.set("peer_id", local_peer_id);
            } else if (op == "find_peer" || op == "store") {
              if (!limiter.Allow(remote_peer)) {
                {
                  std::lock_guard lock(stats_mutex);
                  ++stats.inbound_rate_limited;
                }
                response = MakeErrorResponse(req_id, "rate_limited", "dht rate limited");
              } else if (op == "find_peer") {
                          {
                            std::lock_guard lock(stats_mutex);
                            ++stats.inbound_find_peer;
                          }
                          response.set("op", "find_peer_result");
                          response.set("req_id", req_id);
                          response.set("version", int64_t{kDhtWireVersion});
                          const std::string peer_id = root->getString("peer_id").value_or("");
                          response.set("peer_id", peer_id);
                          std::vector<PeerRoutingRecord> records;
                          if (auto local = store->Get(peer_id)) {
                            records.push_back(*local);
                          }
                          response.set("records", RecordsToJsonArray(records));
                          response.set("closer_peers", makeArray(std::vector<Value>{}));
                        } else {
                          {
                            std::lock_guard lock(stats_mutex);
                            ++stats.inbound_store;
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
                                } else if (auto existing = store->Get(parsed->peer_id);
                                           existing && existing->seq > parsed->seq) {
                                  reject_code = "seq_regression";
                                } else if (!store->Put(*parsed)) {
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
                            std::lock_guard lock(stats_mutex);
                            ++stats.store_rejected;
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
                      reply->Send(JsonToBody(DumpJson(response)));
                      reply->Close();
                    });
          return true;
        });
  }

};

DhtServer::DhtServer(pp::amp::MeshRuntime& runtime, DhtRecordStore& store, WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
  impl_->store = &store;
  impl_->post_worker = std::move(post_worker);
}

DhtServer::~DhtServer() { Stop(); }

void DhtServer::Configure(const AmpDhtProtocolConfig& config) {
  impl_->local_peer_id = config.local_peer_id;
  impl_->limiter.Configure(config.tunables.inbound_ops_per_peer_per_window,
                           config.tunables.inbound_rate_window_seconds);
}

void DhtServer::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  runtime_.Links().SetProtocolHandler(
      kDhtProtocolId,
      impl_->deferred.Bind([impl = impl_.get()](pp::amp::LinkHandle handle, const std::string& remote_peer_id,
                                                const uint32_t channel_id) {
        impl->HandleInboundOnLink(handle, remote_peer_id, channel_id);
      }));
}

void DhtServer::Stop() {
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.Links().RemoveProtocolHandler(kDhtProtocolId);
  impl_->deferred.Invalidate();
}

DhtServer::Stats DhtServer::InboundStats() const {
  std::lock_guard lock(impl_->stats_mutex);
  return impl_->stats;
}

} // namespace pbr
