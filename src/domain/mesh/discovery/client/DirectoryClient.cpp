#include "domain/mesh/discovery/client/DirectoryClient.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "domain/mesh/discovery/MeshNodeHitCodec.h"
#include "common/SettledWait.h"
#include "common/Utilities.h"
#include "domain/mesh/shared/AmpChannelOpen.h"
#include "domain/mesh/shared/AmpParkUntil.h"

#include <chrono>
#include <mutex>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

std::chrono::milliseconds ControlTimeout(const AmpDirectoryProtocolConfig& config) {
  const int ms = config.rpc_timeout_ms > 0 ? config.rpc_timeout_ms : 5000;
  return std::chrono::milliseconds(ms);
}

} // namespace

DirectoryClient::Failure DirectoryClient::WrapLinkFailure(
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

DirectoryClient::DirectoryClient(pp::amp::MeshRuntime& runtime, IoPump io_pump)
    : runtime_(runtime), io_pump_(std::move(io_pump)) {}

void DirectoryClient::Configure(const AmpDirectoryProtocolConfig& config) { config_ = config; }

void DirectoryClient::Start() { stopped_.store(false, std::memory_order_release); }

void DirectoryClient::Stop() { stopped_.store(true, std::memory_order_release); }

void DirectoryClient::Rpc(const std::string& peer_key, Object request, std::function<void(RpcRoe)> on_response) {
  if (stopped_.load(std::memory_order_acquire)) {
    on_response(RpcRoe::error(Failure::Of(Err::NotStarted, "directory service stopped")));
    return;
  }
  if (!runtime_.Links().GetLinkSnapshot(peer_key).has_endpoint) {
    on_response(
        RpcRoe::error(Failure::Of(Err::EndpointNotRegistered, "directory peer endpoint not registered")));
    return;
  }
  const auto timeout = ControlTimeout(config_);
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
    runtime_.Links().OpenChannel(peer_key, kDirectoryProtocolId, pp::amp::ControlJsonChannelPolicy(timeout),
                       [this, peer_key, request_json, finish, session_holder, deadline,
                        timeout](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
                         if (!channel) {
                           finish(RpcRoe::error(WrapLinkFailure(channel.error())));
                           return;
                         }
                         if (stopped_.load(std::memory_order_acquire)) {
                           finish(RpcRoe::error(Failure::Of(Err::NotStarted, "directory service stopped")));
                           return;
                         }
                         const uint32_t channel_id = *channel;
                         AmpWhenChannelOpen(
                             runtime_.Links(), peer_key, channel_id, deadline,
                             [this, peer_key, channel_id, request_json, finish, session_holder, timeout](
                                 bool open) mutable {
                               if (stopped_.load(std::memory_order_acquire)) {
                                 finish(RpcRoe::error(
                                     Failure::Of(Err::NotStarted, "directory service stopped")));
                                 return;
                               }
                               if (!open) {
                                 finish(RpcRoe::error(
                                     Failure::Of(Err::ChannelFailed, "directory channel open failed")));
                                 return;
                               }
                               *session_holder = runtime_.Links().BindChannel(
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
                                   },
                                   // The reply channel can end with no frame (read timeout, link
                                   // dropped, server stopping): settle, or the request never does.
                                   [finish](const char* reason) {
                                     finish(RpcRoe::error(Failure::Of(
                                         Err::ChannelFailed, std::string("directory channel closed: ") +
                                                                 (reason ? reason : ""))));
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

void DirectoryClient::ListMeshNodesAsync(std::function<void(ListRoe)> on_done) {
  if (stopped_.load(std::memory_order_acquire)) {
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
    Rpc(peer_key, request, [state, on_done](RpcRoe resp) mutable {
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

DirectoryClient::ListRoe DirectoryClient::ListMeshNodes() {
  if (stopped_.load(std::memory_order_acquire)) {
    return ListRoe::error(Failure::Of(Err::NotStarted, "directory service not started"));
  }
  SettledWait<std::vector<MeshNodeHit>, Failure> wait;
  ListMeshNodesAsync([&](ListRoe value) { wait.Finish(std::move(value)); });

  const auto timeout = ControlTimeout(config_) + std::chrono::milliseconds(3000);
  const auto deadline = Clock::now() + timeout;
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Failure::Of(Err::Timeout, "directory list timed out"));
}

} // namespace pbr
