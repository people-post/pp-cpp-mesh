#pragma once

#include "base/mesh/dht/DhtRecordStore.h"
#include "base/mesh/dht/DhtTypes.h"

#include "amp/link/PeerLinkManager.h"
#include "common/Error.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Amp L4 mesh DHT (`/pp-mesh/dht/1.0.0`) — v1 FIND_PEER + self STORE.
 * Thin bootstrap fan-out (no full routing table yet).
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

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::PeerLinkManager& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  DhtRecordStore store_;
  AmpDhtServiceConfig config_;
  bool started_ = false;
  int64_t self_seq_ = 0;
  std::chrono::steady_clock::time_point next_self_publish_{};
};

} // namespace pbr
