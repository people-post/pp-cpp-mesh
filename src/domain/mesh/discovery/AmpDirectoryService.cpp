#include "domain/mesh/discovery/AmpDirectoryService.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "domain/mesh/discovery/MeshNodeHitCodec.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <chrono>
#include <condition_variable>
#include <optional>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const AmpDirectoryService::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

std::chrono::milliseconds ControlTimeout(const AmpDirectoryServiceConfig& config) {
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

AmpDirectoryService::Failure AmpDirectoryService::WrapLinkFailure(
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

struct AmpDirectoryService::Impl {
  pp::amp::PeerLinkManager* links = nullptr;
  AmpDirectoryService* self = nullptr;
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
                    RunWorker(post_worker, [this, session, body = std::move(body),
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
                      if (version != kDirectoryWireVersion) {
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
      on_response(RpcRoe::error(Failure::Of(Err::NotStarted, "directory service stopped")));
      return;
    }
    if (!links->GetLinkSnapshot(peer_key).has_endpoint) {
      on_response(
          RpcRoe::error(Failure::Of(Err::EndpointNotRegistered, "directory peer endpoint not registered")));
      return;
    }
    const auto timeout = ControlTimeout(self->config_);
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
      links->OpenChannel(peer_key, kDirectoryProtocolId, pp::amp::ControlJsonChannelPolicy(timeout),
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
                             finish(RpcRoe::error(
                                 Failure::Of(Err::ChannelFailed, "directory channel open failed")));
                             return;
                           }
                           session->Bind(*link->Mux(), *channel, pp::amp::ControlJsonChannelPolicy(timeout),
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
                           if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                             finish(RpcRoe::error(
                                 Failure::Of(Err::ProtocolError, "directory request send failed")));
                             return;
                           }
                           if (io_pump) {
                             io_pump();
                           }
                         });
    });
  }
};

AmpDirectoryService::AmpDirectoryService(pp::amp::PeerLinkManager& links, IoPump io_pump,
                                         WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
  impl_->self = this;
}

AmpDirectoryService::~AmpDirectoryService() { Stop(); }

void AmpDirectoryService::Configure(AmpDirectoryServiceConfig config) {
  config_ = std::move(config);
  inbound_limiter_.Configure(config_.inbound_ops_per_peer_per_window > 0
                                 ? config_.inbound_ops_per_peer_per_window
                                 : 30,
                             config_.inbound_rate_window_seconds > 0 ? config_.inbound_rate_window_seconds
                                                                    : 10);
}

void AmpDirectoryService::SetNodesProvider(AmpDirectoryNodesProvider provider) {
  std::lock_guard lock(nodes_mutex_);
  nodes_provider_ = std::move(provider);
}

void AmpDirectoryService::SetNodesSnapshot(std::vector<MeshNodeHit> nodes) {
  std::lock_guard lock(nodes_mutex_);
  nodes_snapshot_ = std::move(nodes);
}

std::vector<MeshNodeHit> AmpDirectoryService::LocalNodes() const {
  std::lock_guard lock(nodes_mutex_);
  if (nodes_provider_) {
    return nodes_provider_();
  }
  return nodes_snapshot_;
}

bool AmpDirectoryService::AllowInbound(const std::string& remote_peer) {
  return inbound_limiter_.Allow(remote_peer);
}

void AmpDirectoryService::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kDirectoryProtocolId,
                            [impl = impl_.get()](pp::amp::PeerLink& link, const uint32_t channel_id) {
                              impl->HandleInboundOnLink(link, channel_id);
                            });
}

void AmpDirectoryService::Stop() {
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kDirectoryProtocolId);
}

void AmpDirectoryService::ListMeshNodesAsync(std::function<void(ListRoe)> on_done) {
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

AmpDirectoryService::ListRoe AmpDirectoryService::ListMeshNodes() {
  if (!started_) {
    return ListRoe::error(Failure::Of(Err::NotStarted, "directory service not started"));
  }
  std::mutex mutex;
  std::condition_variable cv;
  bool settled = false;
  ListRoe result = ListRoe::error(Failure::Of(Err::Timeout, "directory list timed out"));

  ListMeshNodesAsync([&](ListRoe value) {
    {
      std::lock_guard lock(mutex);
      result = std::move(value);
      settled = true;
    }
    cv.notify_one();
  });

  const auto timeout = ControlTimeout(config_) + std::chrono::milliseconds(3000);
  const auto deadline = Clock::now() + timeout;
  while (true) {
    {
      std::unique_lock lock(mutex);
      if (settled) {
        return result;
      }
    }
    if (Clock::now() >= deadline) {
      break;
    }
    if (io_pump_) {
      io_pump_();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::unique_lock lock(mutex);
    cv.wait_for(lock, std::chrono::milliseconds(1), [&] { return settled; });
  }
  return ListRoe::error(Failure::Of(Err::Timeout, "directory list timed out"));
}

AmpDirectoryClient::AmpDirectoryClient(AmpDirectoryService& service) : service_(service) {}

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
