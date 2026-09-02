#include "base/mesh/dht/AmpDhtService.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "base/mesh/dht/DhtRecordCodec.h"
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
                      } else if (op == "find_peer") {
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
                      } else if (op == "store") {
                        bool ok = false;
                        if (const Object* record_obj = root->getObject("record")) {
                          if (auto parsed = PeerRoutingRecordFromObject(*record_obj)) {
                            if (parsed->peer_id == remote_peer) {
                              if (auto verified = VerifyPeerRoutingRecord(*parsed, remote_pk)) {
                                if (*verified) {
                                  ok = self->store_.Put(*parsed);
                                }
                              }
                            }
                          }
                        }
                        response.set("op", "store_result");
                        response.set("req_id", req_id);
                        response.set("version", int64_t{kDhtWireVersion});
                        response.set("ok", ok);
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

  void Rpc(const std::string& peer_key, Object request, std::function<void(Roe<Object>)> on_response) {
    if (stopped.load(std::memory_order_acquire) || !links) {
      on_response(Error("dht service stopped"));
      return;
    }
    if (!links->GetLinkSnapshot(peer_key).has_endpoint) {
      on_response(Error("dht peer endpoint not registered"));
      return;
    }
    const auto timeout = ControlTimeout(self->config_.tunables);
    const auto deadline = Clock::now() + timeout + std::chrono::milliseconds(2000);
    const std::string request_json = DumpJson(request);
    auto session = std::make_shared<pp::amp::ChannelSession>();
    auto settled = std::make_shared<std::atomic<bool>>(false);

    auto finish = [settled, session, on_response = std::move(on_response)](Roe<Object> value) {
      if (settled->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      session->Close();
      on_response(std::move(value));
    };

    links->EnsureAssociation(peer_key, [this, peer_key, request_json, finish, session, deadline,
                                        timeout](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
      if (!assoc) {
        finish(Error(assoc.error().message));
        return;
      }
      links->OpenChannel(peer_key, kDhtProtocolId, pp::amp::ControlJsonChannelPolicy(timeout),
                         [this, peer_key, request_json, finish, session, deadline,
                          timeout](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
                           if (!channel) {
                             finish(Error(channel.error().message));
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
                             finish(Error("dht channel open failed"));
                             return;
                           }
                           session->Bind(*link->Mux(), *channel, pp::amp::ControlJsonChannelPolicy(timeout),
                                         [finish](Roe<std::vector<uint8_t>> frame) {
                                           if (!frame) {
                                             finish(Error("dht response read failed"));
                                             return false;
                                           }
                                           auto root = TryParseObject(std::string(frame->begin(), frame->end()));
                                           if (!root) {
                                             finish(Error("invalid dht response json"));
                                             return false;
                                           }
                                           finish(std::move(*root));
                                           return false;
                                         });
                           if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                             finish(Error("dht request send failed"));
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
    impl_->Rpc(peer_key, store_req, [](Roe<Object>) {});
  }
}

void AmpDhtService::FindPeer(const std::string& target_peer_id,
                             std::function<void(Roe<DhtFindPeerResult>)> on_done) {
  if (!started_) {
    on_done(Error("dht service not started"));
    return;
  }
  if (target_peer_id.empty()) {
    on_done(Error("missing target peer_id"));
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
  if (config_.query_peer_keys.empty()) {
    on_done(Error("no dht query peers configured"));
    return;
  }

  struct State {
    std::mutex mutex;
    std::shared_ptr<std::atomic<size_t>> pending;
    std::optional<PeerRoutingRecord> best;
    std::string last_error;
  };
  auto state = std::make_shared<State>();
  state->pending = std::make_shared<std::atomic<size_t>>(config_.query_peer_keys.size());

  Object request;
  request.set("op", "find_peer");
  request.set("req_id", util::GenerateUuid());
  request.set("version", int64_t{kDhtWireVersion});
  request.set("peer_id", target_peer_id);

  for (const std::string& peer_key : config_.query_peer_keys) {
    impl_->Rpc(peer_key, request, [state, target_peer_id, on_done](Roe<Object> resp) mutable {
      {
        std::lock_guard lock(state->mutex);
        if (resp) {
          if (const Array* records = resp->getArray("records")) {
            for (const Value& item : records->elements) {
              const Object* obj = asObject(item);
              if (!obj) {
                continue;
              }
              if (auto parsed = PeerRoutingRecordFromObject(*obj)) {
                if (parsed->peer_id != target_peer_id) {
                  continue;
                }
                if (!state->best || parsed->seq > state->best->seq) {
                  state->best = std::move(*parsed);
                }
              }
            }
          }
        } else {
          state->last_error = resp.error().message;
        }
      }
      if (state->pending->fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
      }
      std::lock_guard lock(state->mutex);
      if (state->best) {
        DhtFindPeerResult result;
        result.peer_id = target_peer_id;
        result.record = *state->best;
        on_done(std::move(result));
        return;
      }
      on_done(Error(state->last_error.empty() ? "find_peer not found" : state->last_error));
    });
  }
}

std::optional<PeerRoutingRecord> AmpDhtService::LocalRecord(const std::string& peer_id) const {
  return store_.Get(peer_id);
}

} // namespace pbr
