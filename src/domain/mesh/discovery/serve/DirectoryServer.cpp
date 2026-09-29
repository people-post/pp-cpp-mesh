#include "domain/mesh/discovery/serve/DirectoryServer.h"

#include "domain/mesh/l4/shared/InboundReply.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "domain/mesh/discovery/MeshNodeHitCodec.h"
#include "common/ValueJson.h"
#include "foundation/runtime/DeferredSelf.h"

#include <atomic>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const DirectoryServer::WorkerPost& post_worker, std::function<void()> task) {
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
  object.set("version", int64_t{kDirectoryWireVersion});
  object.set("code", code);
  object.set("message", message);
  return object;
}

} // namespace

struct DirectoryServer::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  WorkerPost post_worker;
  std::atomic<bool> stopped{false};
  /** Guards protocol-handler raw Impl* past Stop — OWNERSHIP.md § DeferredSelf. */
  DeferredSelf deferred;
  DhtRateLimiter limiter;
  mutable std::mutex mu;  // local_peer_id, provider, snapshot
  std::string local_peer_id;
  AmpDirectoryNodesProvider nodes_provider;
  std::vector<MeshNodeHit> nodes_snapshot;

  pp::amp::PeerLinkManager& Links() { return runtime->Links(); }
  /** IO lane for InboundReply. */
  InboundReply::IoPost IoPost() {
    return [rt = runtime](std::function<void()> task) { rt->PostToIo(std::move(task)); };
  }

  std::string LocalPeerId() const {
    std::lock_guard lock(mu);
    return local_peer_id;
  }

  std::vector<MeshNodeHit> LocalNodes() const {
    std::lock_guard lock(mu);
    if (nodes_provider) {
      return nodes_provider();
    }
    return nodes_snapshot;
  }

  void HandleInboundOnLink(pp::amp::LinkHandle /*handle*/, const std::string& remote_peer_id,
                           const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime || remote_peer_id.empty()) {
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
                        response.set("peer_id", LocalPeerId());
                      } else if (op == "list_mesh_nodes") {
                        if (!limiter.Allow(remote_peer)) {
                          response = MakeErrorResponse(req_id, "rate_limited", "directory rate limited");
                        } else {
                          response.set("op", "list_mesh_nodes_result");
                          response.set("req_id", req_id);
                          response.set("version", int64_t{kDirectoryWireVersion});
                          response.set("nodes", MeshNodeHitsToJsonArray(LocalNodes()));
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

DirectoryServer::DirectoryServer(pp::amp::MeshRuntime& runtime, WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
  impl_->post_worker = std::move(post_worker);
}

DirectoryServer::~DirectoryServer() { Stop(); }

void DirectoryServer::Configure(const AmpDirectoryProtocolConfig& config) {
  {
    std::lock_guard lock(impl_->mu);
    impl_->local_peer_id = config.local_peer_id;
  }
  impl_->limiter.Configure(config.inbound_ops_per_peer_per_window > 0 ? config.inbound_ops_per_peer_per_window : 30,
                           config.inbound_rate_window_seconds > 0 ? config.inbound_rate_window_seconds : 10);
}

void DirectoryServer::SetNodesProvider(AmpDirectoryNodesProvider provider) {
  std::lock_guard lock(impl_->mu);
  impl_->nodes_provider = std::move(provider);
}

void DirectoryServer::SetNodesSnapshot(std::vector<MeshNodeHit> nodes) {
  std::lock_guard lock(impl_->mu);
  impl_->nodes_snapshot = std::move(nodes);
}

void DirectoryServer::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  runtime_.Links().SetProtocolHandler(
      kDirectoryProtocolId,
      impl_->deferred.Bind([impl = impl_.get()](pp::amp::LinkHandle handle, const std::string& remote_peer_id,
                                                const uint32_t channel_id) {
        impl->HandleInboundOnLink(handle, remote_peer_id, channel_id);
      }));
}

void DirectoryServer::Stop() {
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.Links().RemoveProtocolHandler(kDirectoryProtocolId);
  impl_->deferred.Invalidate();
}

} // namespace pbr
