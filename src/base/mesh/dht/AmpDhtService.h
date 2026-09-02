#pragma once

#include "base/mesh/dht/DhtRateLimiter.h"
#include "base/mesh/dht/DhtRecordStore.h"
#include "base/mesh/dht/DhtTypes.h"

#include "amp/link/PeerLinkManager.h"
#include "common/Error.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Amp L4 mesh DHT (`/pp-mesh/dht/1.0.0`) — FIND_PEER + self STORE.
 * Thin bootstrap fan-out with n2-hard rate limits + soft reputation.
 */
class AmpDhtService {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;

  AmpDhtService(pp::amp::PeerLinkManager& links, IoPump io_pump = {}, WorkerPost post_worker = {});
  ~AmpDhtService();

  AmpDhtService(const AmpDhtService&) = delete;
  AmpDhtService& operator=(const AmpDhtService&) = delete;

  void Configure(AmpDhtServiceConfig config);
  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /** Periodic: refresh self record + push to bootstrap peers. */
  void Tick();

  void FindPeer(const std::string& target_peer_id, std::function<void(Roe<DhtFindPeerResult>)> on_done);

  std::optional<PeerRoutingRecord> LocalRecord(const std::string& peer_id) const;
  std::vector<PeerRoutingRecord> SnapshotRecords() const;
  DhtOpsStats Stats() const;
  std::string FormatOpsStatusJson() const;

private:
  friend struct Impl;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::PeerLinkManager& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  DhtRecordStore store_;
  DhtRateLimiter inbound_limiter_;
  AmpDhtServiceConfig config_;
  bool started_ = false;
  int64_t self_seq_ = 0;
  std::chrono::steady_clock::time_point next_self_publish_{};

  mutable std::mutex stats_mutex_;
  uint64_t inbound_find_peer_ = 0;
  uint64_t inbound_store_ = 0;
  uint64_t inbound_rate_limited_ = 0;
  uint64_t store_rejected_ = 0;
  uint64_t find_peer_issued_ = 0;
  uint64_t soft_reputation_skips_ = 0;

  mutable std::mutex reputation_mutex_;
  struct SoftRep {
    int bad_count = 0;
    std::chrono::steady_clock::time_point cooldown_until{};
  };
  std::unordered_map<std::string, SoftRep> soft_reputation_;

  std::atomic<int> inflight_lookups_{0};

  bool AllowInbound(const std::string& remote_peer);
  void NoteSoftReputationBad(const std::string& peer_key);
  bool SoftReputationAllows(const std::string& peer_key) const;
  std::vector<std::string> FilteredQueryPeerKeys();
};

} // namespace pbr
