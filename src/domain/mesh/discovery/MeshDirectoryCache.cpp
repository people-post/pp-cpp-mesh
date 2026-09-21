#include "domain/mesh/discovery/MeshDirectoryCache.h"

#include "domain/mesh/discovery/NameDirectory.h"
#include "foundation/data/AtomicFileWrite.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/ValueJson.h"

#include <fstream>
#include <memory>
#include <optional>

namespace pbr {
namespace {

constexpr int kMeshDirectoryCacheSchemaVersion = 1;

Object NodeToObject(const MeshDirectoryNode& node) {
  Object o;
  o.set("peer_id", node.peer_id);
  o.set("account_id", node.account_id);
  o.set("nickname", node.nickname);
  o.set("entity_kind", node.entity_kind.empty() ? "mesh_node" : node.entity_kind);
  o.set("seq", node.seq);
  o.set("expires_at", node.expires_at);
  o.set("circuit_relay", node.circuit_relay);
  o.set("media_relay", node.media_relay);
  o.set("dht", node.dht);
  o.set("ledger_gateway", node.ledger_gateway);
  std::vector<Value> mas;
  mas.reserve(node.multiaddrs.size());
  for (const std::string& ma : node.multiaddrs) {
    mas.emplace_back(ma);
  }
  o.set("multiaddrs", makeArray(std::move(mas)));
  return o;
}

std::optional<MeshDirectoryNode> NodeFromObject(const Object& o) {
  MeshDirectoryNode node;
  node.peer_id = o.getString("peer_id").value_or("");
  if (node.peer_id.empty()) {
    return std::nullopt;
  }
  node.account_id = o.getString("account_id").value_or("");
  node.nickname = o.getString("nickname").value_or("");
  node.entity_kind = o.getString("entity_kind").value_or("mesh_node");
  node.seq = o.getIf<int64_t>("seq").value_or(0);
  node.expires_at = o.getString("expires_at").value_or("");
  node.circuit_relay = o.getIf<bool>("circuit_relay").value_or(false);
  node.media_relay = o.getIf<bool>("media_relay").value_or(false);
  node.dht = o.getIf<bool>("dht").value_or(false);
  node.ledger_gateway = o.getIf<bool>("ledger_gateway").value_or(false);
  if (const Array* mas = o.getArray("multiaddrs")) {
    for (const Value& v : mas->elements) {
      if (auto s = asString(v)) {
        if (!s->empty()) {
          node.multiaddrs.push_back(*s);
        }
      }
    }
  }
  return node;
}

} // namespace

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

void MeshDirectoryCache::SetAsyncFetcher(AsyncFetcher fetcher) {
  std::lock_guard lock(mutex_);
  async_fetcher_ = std::move(fetcher);
}

void MeshDirectoryCache::SetPersistPath(std::string path) {
  std::lock_guard lock(mutex_);
  persist_path_ = std::move(path);
}

void MeshDirectoryCache::LoadPersisted() {
  std::string path;
  {
    std::lock_guard lock(mutex_);
    if (persist_path_.empty() || !nodes_.empty()) {
      return;
    }
    path = persist_path_;
  }
  std::ifstream in(path);
  if (!in) {
    return;
  }
  const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto root = TryParseObject(body);
  if (!root) {
    return;
  }
  const int version = static_cast<int>(root->getIf<int64_t>("schema_version").value_or(0));
  if (version != kMeshDirectoryCacheSchemaVersion) {
    return;
  }
  const Array* arr = root->getArray("nodes");
  if (!arr) {
    return;
  }
  std::vector<MeshDirectoryNode> loaded;
  loaded.reserve(arr->elements.size());
  for (const Value& v : arr->elements) {
    const Object* o = asObject(v);
    if (!o) {
      continue;
    }
    if (auto node = NodeFromObject(*o)) {
      loaded.push_back(std::move(*node));
    }
  }
  if (loaded.empty()) {
    return;
  }
  std::function<void()> notify;
  {
    std::lock_guard lock(mutex_);
    if (!nodes_.empty()) {
      return;
    }
    nodes_ = std::move(loaded);
    notify = on_updated_;
  }
  if (notify) {
    notify();
  }
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

void MeshDirectoryCache::PersistLocked() const {
  if (persist_path_.empty() || nodes_.empty()) {
    return;
  }
  Object root;
  root.set("schema_version", int64_t{kMeshDirectoryCacheSchemaVersion});
  std::vector<Value> rows;
  rows.reserve(nodes_.size());
  for (const MeshDirectoryNode& node : nodes_) {
    rows.emplace_back(std::make_shared<Object>(NodeToObject(node)));
  }
  root.set("nodes", makeArray(std::move(rows)));
  (void)AtomicFileWrite::Write(persist_path_, DumpJson(root, 2));
}

void MeshDirectoryCache::ApplyRefreshResult(Roe<std::vector<MeshDirectoryNode>> result) {
  std::function<void()> notify;
  {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex_);
    inflight_ = false;
    if (result) {
      nodes_ = std::move(*result);
      next_refresh_at_ = now + refresh_interval_;
      PersistLocked();
    } else {
      next_refresh_at_ = now + failure_backoff_;
    }
    notify = on_updated_;
  }
  if (notify) {
    notify();
  }
}

void MeshDirectoryCache::RequestRefresh() {
  AsyncFetcher async;
  Fetcher sync;
  {
    std::lock_guard lock(mutex_);
    if (inflight_) {
      return;
    }
    if (!async_fetcher_ && !fetcher_) {
      return;
    }
    inflight_ = true;
    async = async_fetcher_;
    sync = fetcher_;
  }

  if (async) {
    async([this](Roe<std::vector<MeshDirectoryNode>> result) {
      AppRuntime::PostUI([this, result = std::move(result)]() mutable { ApplyRefreshResult(std::move(result)); });
    });
    return;
  }

  AppRuntime::PostWorkerAndReplyOnUI<Roe<std::vector<MeshDirectoryNode>>>(
      WorkerLane::Normal, [sync]() { return sync(); },
      [this](Roe<std::vector<MeshDirectoryNode>> result) { ApplyRefreshResult(std::move(result)); });
}

std::vector<MeshDirectoryNode> MeshDirectoryNodesFromHits(const std::vector<MeshNodeHit>& hits) {
  return MeshDirectoryNodesFromNameRecords(NameRecordsFromMeshNodeHits(hits));
}

} // namespace pbr
