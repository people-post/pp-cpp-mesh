#pragma once

#include "foundation/data/MeshConfigTypes.h"
#include "common/directory/DirectoryTypes.h"
#include "common/Error.h"
#include "common/Executor.h"
#include "common/Lifetime.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Periodic cache of Brief mesh_node listings (n-dir).
 * Fetcher is injected so base/mesh stays free of HTTP client wiring.
 *
 * A refresh runs on `blocking` (the sync fetcher waits on HTTP; persisting writes a file) and lands
 * there: `on_updated` runs on that thread — the app moves its own work to its thread (THREADING.md
 * § Mesh threads, rule 2). Destruction drops a refresh still in flight.
 */
class MeshDirectoryCache {
public:
  using Fetcher = std::function<Roe<std::vector<MeshDirectoryNode>>()>;
  /** Completion may run on any thread; the result is applied on `blocking`. */
  using AsyncFetcher = std::function<void(std::function<void(Roe<std::vector<MeshDirectoryNode>>)>)>;

  /** `blocking`: the mesh's executor for long waits (null: inline, tests). */
  MeshDirectoryCache(Fetcher fetcher, std::shared_ptr<pp::Executor> blocking);
  /** Waits for a result being applied on another thread; a fetch in progress is dropped when done. */
  ~MeshDirectoryCache();
  MeshDirectoryCache(const MeshDirectoryCache&) = delete;
  MeshDirectoryCache& operator=(const MeshDirectoryCache&) = delete;

  void SetRefreshInterval(std::chrono::seconds interval);
  void SetFailureBackoff(std::chrono::seconds backoff);
  void SetOnUpdated(std::function<void()> callback);
  /** When set, RequestRefresh prefers this over parking a worker on sync Fetcher. */
  void SetAsyncFetcher(AsyncFetcher fetcher);
  /**
   * Machine-wide last-good mesh_node list (N027 cold start before HTTP).
   * Call LoadPersisted() after set so Snapshot is non-empty before first refresh.
   */
  void SetPersistPath(std::string path);
  void LoadPersisted();

  /** Thread-safe snapshot for hop policy (may be empty before first refresh / load). */
  std::vector<MeshDirectoryNode> Snapshot() const;

  /** Cheap due-check; schedules a refresh when the interval elapsed. */
  void MaybeRefresh();

  /** Force a refresh (no-op while in flight). */
  void RequestRefresh();

private:
  void ApplyRefreshResult(Roe<std::vector<MeshDirectoryNode>> result);
  void PersistLocked() const;

  Fetcher fetcher_;
  AsyncFetcher async_fetcher_;
  mutable std::mutex mutex_;
  std::vector<MeshDirectoryNode> nodes_;
  std::string persist_path_;
  std::chrono::steady_clock::time_point next_refresh_at_{};
  std::chrono::seconds refresh_interval_{300};
  std::chrono::seconds failure_backoff_{60};
  bool inflight_ = false;
  std::function<void()> on_updated_;
  const std::shared_ptr<pp::Executor> blocking_;
  /** Last: invalidated first, before the fields a late result would touch. */
  pp::Lifetime lifetime_;
};

/** Flatten Brief mesh_node hits into per-endpoint rows for hop policy. */
std::vector<MeshDirectoryNode> MeshDirectoryNodesFromHits(const std::vector<MeshNodeHit>& hits);

} // namespace pbr
