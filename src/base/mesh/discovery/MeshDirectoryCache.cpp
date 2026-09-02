#include "base/mesh/discovery/MeshDirectoryCache.h"

#include "base/net/ServiceClients.h"
#include "base/runtime/AppRuntime.h"

namespace pbr {

MeshDirectoryCache::MeshDirectoryCache(Fetcher fetcher) : fetcher_(std::move(fetcher)) {}

void MeshDirectoryCache::SetRefreshInterval(std::chrono::seconds interval) {
  std::lock_guard lock(mutex_);
  refresh_interval_ = interval;
}

void MeshDirectoryCache::SetFailureBackoff(std::chrono::seconds backoff) {
  std::lock_guard lock(mutex_);
  failure_backoff_ = backoff;
}

void MeshDirectoryCache::SetOnUpdated(std::function<void()> callback) {
  std::lock_guard lock(mutex_);
  on_updated_ = std::move(callback);
}

std::vector<MeshDirectoryNode> MeshDirectoryCache::Snapshot() const {
  std::lock_guard lock(mutex_);
  return nodes_;
}

void MeshDirectoryCache::MaybeRefresh() {
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard lock(mutex_);
    if (inflight_ || now < next_refresh_at_) {
      return;
    }
  }
  RequestRefresh();
}

void MeshDirectoryCache::RequestRefresh() {
  if (!fetcher_) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (inflight_) {
      return;
    }
    inflight_ = true;
  }

  AppRuntime::PostWorkerAndReplyOnUI<Roe<std::vector<MeshDirectoryNode>>>(
      WorkerLane::Normal, [this]() { return fetcher_(); },
      [this](Roe<std::vector<MeshDirectoryNode>> result) {
        std::function<void()> notify;
        {
          const auto now = std::chrono::steady_clock::now();
          std::lock_guard lock(mutex_);
          inflight_ = false;
          if (result) {
            nodes_ = std::move(*result);
            next_refresh_at_ = now + refresh_interval_;
          } else {
            next_refresh_at_ = now + failure_backoff_;
          }
          notify = on_updated_;
        }
        if (notify) {
          notify();
        }
      });
}

std::vector<MeshDirectoryNode> MeshDirectoryNodesFromHits(const std::vector<MeshNodeHit>& hits) {
  std::vector<MeshDirectoryNode> out;
  for (const MeshNodeHit& hit : hits) {
    for (const DirectoryEndpoint& ep : hit.endpoints) {
      if (ep.peer_id.empty()) {
        continue;
      }
      MeshDirectoryNode node;
      node.peer_id = ep.peer_id;
      node.multiaddrs = ep.multiaddrs;
      node.circuit_relay = hit.capabilities.circuit_relay;
      node.media_relay = hit.capabilities.media_relay;
      out.push_back(std::move(node));
    }
  }
  return out;
}

} // namespace pbr
