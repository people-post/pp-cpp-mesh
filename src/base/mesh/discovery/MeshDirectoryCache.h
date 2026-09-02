#pragma once

#include "base/data/Config.h"
#include "base/net/ServiceClients.h"
#include "common/Error.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Periodic cache of Brief mesh_node listings (n-dir).
 * Fetcher is injected so base/mesh stays free of HTTP client wiring.
 */
class MeshDirectoryCache {
public:
  using Fetcher = std::function<Roe<std::vector<MeshDirectoryNode>>()>;

  explicit MeshDirectoryCache(Fetcher fetcher);

  void SetRefreshInterval(std::chrono::seconds interval);
  void SetFailureBackoff(std::chrono::seconds backoff);
  void SetOnUpdated(std::function<void()> callback);

  /** Thread-safe snapshot for hop policy (may be empty before first refresh). */
  std::vector<MeshDirectoryNode> Snapshot() const;

  /** Cheap due-check; schedules async refresh when interval elapsed. */
  void MaybeRefresh();

  /** Force async refresh (no-op while inflight). */
  void RequestRefresh();

private:
  Fetcher fetcher_;
  mutable std::mutex mutex_;
  std::vector<MeshDirectoryNode> nodes_;
  std::chrono::steady_clock::time_point next_refresh_at_{};
  std::chrono::seconds refresh_interval_{300};
  std::chrono::seconds failure_backoff_{60};
  bool inflight_ = false;
  std::function<void()> on_updated_;
};

/** Flatten Brief mesh_node hits into per-endpoint rows for hop policy. */
std::vector<MeshDirectoryNode> MeshDirectoryNodesFromHits(const std::vector<MeshNodeHit>& hits);

} // namespace pbr
