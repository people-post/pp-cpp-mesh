#include "domain/mesh/discovery/AmpDirectoryProtocol.h"

#include "domain/mesh/l4/shared/InboundReply.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "domain/mesh/discovery/MeshNodeHitCodec.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "foundation/runtime/DeferredSelf.h"

#include <chrono>
#include <optional>

#include "common/SettledWait.h"
#include "common/PbrCompat.h"
#include "domain/mesh/shared/AmpChannelOpen.h"
#include "domain/mesh/shared/AmpParkUntil.h"

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const AmpDirectoryProtocol::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

std::chrono::milliseconds ControlTimeout(const AmpDirectoryProtocolConfig& config) {
  const int ms = config.rpc_timeout_ms > 0 ? config.rpc_timeout_ms : 5000;
  return std::chrono::milliseconds(ms);
}

Object MakeErrorResponse(const std::string& req_id, const std::string& code, const std::string& message) {
  Object object;
  object.set("op", "error");
  object.set("req_id", req_id);
  object.set("version", int64_t{kDirectoryWireVersion});
  object.set("code", code);
  object.set("message", message);
  return object;
}

} // namespace

AmpDirectoryProtocol::Failure AmpDirectoryProtocol::WrapLinkFailure(
    const pp::amp::PeerLinkManager::Failure& child) {
  switch (child.GetCode()) {
    case pp::amp::PeerLinkManager::Err::EndpointNotRegistered:
      return Failure::Of(Err::EndpointNotRegistered,
                         detail::AppendFrom("directory: endpoint not registered", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialTimeout:
      return Failure::Of(Err::Timeout, detail::AppendFrom("directory: dial timed out", "link", child.message));
    case pp::amp::PeerLinkManager::Err::ChannelOpenFailed:
      return Failure::Of(Err::ChannelFailed,
                         detail::AppendFrom("directory: channel open failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialInBackoff:
    case pp::amp::PeerLinkManager::Err::TooManyConcurrentDials:
    case pp::amp::PeerLinkManager::Err::MaxLinksReached:
    case pp::amp::PeerLinkManager::Err::AssociationNotReady:
    case pp::amp::PeerLinkManager::Err::LinkNotFound:
    case pp::amp::PeerLinkManager::Err::NestedCarrierIncomplete:
    case pp::amp::PeerLinkManager::Err::HandshakeFailed:
    case pp::amp::PeerLinkManager::Err::TransportFailed:
    case pp::amp::PeerLinkManager::Err::DualDialLost:
      return Failure::Of(Err::LinkFailed, detail::AppendFrom("directory: link failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::Ok:
    case pp::amp::PeerLinkManager::Err::Generic:
    default:
      return Failure::Of(Err::Generic, detail::AppendFrom("directory: link error", "link", child.message));
  }
}

struct AmpDirectoryProtocol::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  AmpDirectoryProtocol* self = nullptr;
  WorkerPost post_worker;
  std::atomic<bool> stopped{false};
  /** Guards protocol-handler raw Impl* past Stop — OWNERSHIP.md § DeferredSelf. */
  DeferredSelf deferred;

  pp::amp::PeerLinkManager& Links() { return runtime->Links(); }
  /** IO lane for InboundReply (MeshHost::Stop joins MeshControl before freeing the runtime). */
  InboundReply::IoPost IoPost() {
    return [rt = runtime](std::function<void()> task) { rt->PostToIo(std::move(task)); };
  }

  void HandleInboundOnLink(pp::amp::LinkHandle /*handle*/, const std::string& remote_peer_id,
                           const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime || !self || remote_peer_id.empty()) {
      return;
    }
    const std::string remote_peer = remote_peer_id;
    auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    *session_holder = Links().BindChannel(
        remote_peer_id, channel_id, InboundReplyPolicy(pp::amp::ControlJsonChannelPolicy()),
        [this, session_holder, remote_peer](Roe<std::vector<uint8_t>> frame) {
          auto session = *session_holder;
          if (!session || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          auto body = std::move(*frame);
          // Keep the channel open for the worker's reply (InboundReply.h); `reply` closes it.
          auto reply = MakeInboundReply(session, IoPost());
          RunWorker(post_worker, [this, reply, body = std::move(body), remote_peer]() mutable {
                      if (stopped.load(std::memory_order_acquire) || !self) {
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
                      if (version != kDirectoryWireVersion) {
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
                        response.set("version", int64_t{kDirectoryWireVersion});
                        response.set("peer_id", self->config_.local_peer_id);
                      } else if (op == "list_mesh_nodes") {
                        if (!self->AllowInbound(remote_peer)) {
                          response = MakeErrorResponse(req_id, "rate_limited", "directory rate limited");
                        } else {
                          response.set("op", "list_mesh_nodes_result");
                          response.set("req_id", req_id);
                          response.set("version", int64_t{kDirectoryWireVersion});
                          response.set("nodes", MeshNodeHitsToJsonArray(self->LocalNodes()));
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

  void Rpc(const std::string& peer_key, Object request, std::function<void(RpcRoe)> on_response) {
    if (stopped.load(std::memory_order_acquire) || !runtime) {
      on_response(RpcRoe::error(Failure::Of(Err::NotStarted, "directory service stopped")));
      return;
    }
    if (!Links().GetLinkSnapshot(peer_key).has_endpoint) {
      on_response(
          RpcRoe::error(Failure::Of(Err::EndpointNotRegistered, "directory peer endpoint not registered")));
      return;
    }
    const auto timeout = ControlTimeout(self->config_);
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

    Links().EnsureAssociation(peer_key, [this, peer_key, request_json, finish, session_holder, deadline,
                                        timeout](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
      if (!assoc) {
        finish(RpcRoe::error(WrapLinkFailure(assoc.error())));
        return;
      }
      Links().OpenChannel(peer_key, kDirectoryProtocolId, pp::amp::ControlJsonChannelPolicy(timeout),
                         [this, peer_key, request_json, finish, session_holder, deadline,
                          timeout](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
                           if (!channel) {
                             finish(RpcRoe::error(WrapLinkFailure(channel.error())));
                             return;
                           }
                           if (stopped.load(std::memory_order_acquire)) {
                             finish(RpcRoe::error(Failure::Of(Err::NotStarted, "directory service stopped")));
                             return;
                           }
                           const uint32_t channel_id = *channel;
                           AmpWhenChannelOpen(
                               Links(), peer_key, channel_id, deadline,
                               [this, peer_key, channel_id, request_json, finish, session_holder, timeout](
                                   bool open) mutable {
                                 if (stopped.load(std::memory_order_acquire)) {
                                   finish(RpcRoe::error(
                                       Failure::Of(Err::NotStarted, "directory service stopped")));
                                   return;
                                 }
                                 if (!open) {
                                   finish(RpcRoe::error(
                                       Failure::Of(Err::ChannelFailed, "directory channel open failed")));
                                   return;
                                 }
                                 *session_holder = Links().BindChannel(
                                     peer_key, channel_id, pp::amp::ControlJsonChannelPolicy(timeout),
                                     [finish](Roe<std::vector<uint8_t>> frame) {
                                       if (!frame) {
                                         finish(RpcRoe::error(Failure::Of(
                                             Err::ProtocolError, "directory response read failed")));
                                         return false;
                                       }
                                       auto root =
                                           TryParseObject(std::string(frame->begin(), frame->end()));
                                       if (!root) {
                                         finish(RpcRoe::error(Failure::Of(
                                             Err::ProtocolError, "invalid directory response json")));
                                         return false;
                                       }
                                       finish(std::move(*root));
                                       return false;
                                     });
                                 if (!*session_holder) {
                                   finish(RpcRoe::error(
                                       Failure::Of(Err::ChannelFailed, "directory channel open failed")));
                                   return;
                                 }
                                 if (!(*session_holder)->EnqueueOutbound(JsonToBody(request_json))) {
                                   finish(RpcRoe::error(
                                       Failure::Of(Err::ProtocolError, "directory request send failed")));
                                   return;
                                 }
                               });
                         });
    });
  }
};

AmpDirectoryProtocol::AmpDirectoryProtocol(pp::amp::MeshRuntime& runtime, IoPump io_pump,
                                         WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), runtime_(runtime), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->runtime = &runtime_;
  impl_->post_worker = post_worker_;
  impl_->self = this;
}

AmpDirectoryProtocol::~AmpDirectoryProtocol() { Stop(); }

void AmpDirectoryProtocol::Configure(AmpDirectoryProtocolConfig config) {
  config_ = std::move(config);
  inbound_limiter_.Configure(config_.inbound_ops_per_peer_per_window > 0
                                 ? config_.inbound_ops_per_peer_per_window
                                 : 30,
                             config_.inbound_rate_window_seconds > 0 ? config_.inbound_rate_window_seconds
                                                                    : 10);
}

void AmpDirectoryProtocol::SetNodesProvider(AmpDirectoryNodesProvider provider) {
  std::lock_guard lock(nodes_mutex_);
  nodes_provider_ = std::move(provider);
}

void AmpDirectoryProtocol::SetNodesSnapshot(std::vector<MeshNodeHit> nodes) {
  std::lock_guard lock(nodes_mutex_);
  nodes_snapshot_ = std::move(nodes);
}

std::vector<MeshNodeHit> AmpDirectoryProtocol::LocalNodes() const {
  std::lock_guard lock(nodes_mutex_);
  if (nodes_provider_) {
    return nodes_provider_();
  }
  return nodes_snapshot_;
}

bool AmpDirectoryProtocol::AllowInbound(const std::string& remote_peer) {
  return inbound_limiter_.Allow(remote_peer);
}

void AmpDirectoryProtocol::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  runtime_.Links().SetProtocolHandler(
      kDirectoryProtocolId,
      impl_->deferred.Bind([impl = impl_.get()](pp::amp::LinkHandle handle,
                                                const std::string& remote_peer_id,
                                                const uint32_t channel_id) {
        impl->HandleInboundOnLink(handle, remote_peer_id, channel_id);
      }));
}

void AmpDirectoryProtocol::Stop() {
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.Links().RemoveProtocolHandler(kDirectoryProtocolId);
  impl_->deferred.Invalidate();
}

void AmpDirectoryProtocol::ListMeshNodesAsync(std::function<void(ListRoe)> on_done) {
  if (!started_) {
    on_done(ListRoe::error(Failure::Of(Err::NotStarted, "directory service not started")));
    return;
  }
  if (config_.query_peer_keys.empty()) {
    on_done(ListRoe::error(Failure::Of(Err::InvalidRequest, "no directory query peers configured")));
    return;
  }

  struct State {
    std::mutex mutex;
    std::shared_ptr<std::atomic<size_t>> pending;
    std::shared_ptr<std::atomic<bool>> finished;
    std::optional<std::vector<MeshNodeHit>> best;
    Failure last_failure = Failure::Of(Err::NotFound, "directory list_mesh_nodes failed");
  };
  auto state = std::make_shared<State>();
  state->pending = std::make_shared<std::atomic<size_t>>(config_.query_peer_keys.size());
  state->finished = std::make_shared<std::atomic<bool>>(false);

  Object request;
  request.set("op", "list_mesh_nodes");
  request.set("req_id", util::GenerateUuid());
  request.set("version", int64_t{kDirectoryWireVersion});

  for (const std::string& peer_key : config_.query_peer_keys) {
    impl_->Rpc(peer_key, request, [state, on_done](RpcRoe resp) mutable {
      std::optional<ListRoe> outcome;
      {
        std::lock_guard lock(state->mutex);
        if (state->finished->load(std::memory_order_acquire)) {
          return;
        }
        if (resp) {
          const std::string op = resp->getString("op").value_or("");
          if (op == "list_mesh_nodes_result") {
            if (const Array* nodes = resp->getArray("nodes")) {
              if (auto parsed = MeshNodeHitsFromJsonArray(*nodes)) {
                state->best = std::move(*parsed);
                state->finished->store(true, std::memory_order_release);
                outcome = ListRoe(*state->best);
              } else {
                state->last_failure = Failure::Of(Err::ProtocolError, "invalid nodes array");
              }
            } else {
              state->last_failure = Failure::Of(Err::ProtocolError, "missing nodes");
            }
          } else if (op == "error") {
            const std::string message = resp->getString("message").value_or(
                resp->getString("code").value_or("error"));
            state->last_failure = Failure::Of(Err::ProtocolError, message);
          } else {
            state->last_failure = Failure::Of(Err::ProtocolError, "unexpected directory op");
          }
        } else {
          state->last_failure = resp.error();
        }
        if (!outcome && state->pending->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          state->finished->store(true, std::memory_order_release);
          if (state->best) {
            outcome = ListRoe(*state->best);
          } else {
            outcome = ListRoe::error(state->last_failure);
          }
        }
      }
      if (outcome) {
        on_done(std::move(*outcome));
      }
    });
  }
}

AmpDirectoryProtocol::ListRoe AmpDirectoryProtocol::ListMeshNodes() {
  if (!started_) {
    return ListRoe::error(Failure::Of(Err::NotStarted, "directory service not started"));
  }
  SettledWait<std::vector<MeshNodeHit>, Failure> wait;
  ListMeshNodesAsync([&](ListRoe value) { wait.Finish(std::move(value)); });

  const auto timeout = ControlTimeout(config_) + std::chrono::milliseconds(3000);
  const auto deadline = Clock::now() + timeout;
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Failure::Of(Err::Timeout, "directory list timed out"));
}

AmpDirectoryClient::AmpDirectoryClient(AmpDirectoryProtocol& service) : service_(service) {}

Roe<std::vector<DirectoryHit>> AmpDirectoryClient::SearchPeople(const std::string& /*query*/) {
  return Error("Amp directory does not support person search (use HTTP failover)");
}

Roe<DirectoryHit> AmpDirectoryClient::LookupRelayUser(const std::string& /*relay_user_id*/) {
  return Error("Amp directory does not support person lookup (use HTTP failover)");
}

Roe<DirectoryHit> AmpDirectoryClient::LookupByAccount(const std::string& /*account_id*/) {
  return Error("Amp directory does not support account lookup (use HTTP failover)");
}

Roe<std::vector<MeshNodeHit>> AmpDirectoryClient::ListMeshNodes() {
  auto result = service_.ListMeshNodes();
  if (!result) {
    return Error(result.error().message);
  }
  return std::move(*result);
}

} // namespace pbr
