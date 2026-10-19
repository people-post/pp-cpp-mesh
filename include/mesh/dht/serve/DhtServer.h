#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/dht/DhtRateLimiter.h"
#include "domain/mesh/dht/DhtRecordStore.h"
#include "domain/mesh/dht/DhtTypes.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace pbr {

/**
 * Serving side of the Amp mesh DHT (`/pp-mesh/dht/1.0.0`): answers `ping`, `find_peer` from the
 * shared record store and accepts signed self `store`s (the record must be the sender's own,
 * unexpired, not a seq regression), rate limited per remote peer.
 */
class DhtServer {
public:
  using WorkerPost = std::function<void(std::function<void()>)>;

  struct Stats {
    uint64_t inbound_find_peer = 0;
    uint64_t inbound_store = 0;
    uint64_t inbound_rate_limited = 0;
    uint64_t store_rejected = 0;
  };

  DhtServer(pp::amp::MeshRuntime& runtime, DhtRecordStore& store, WorkerPost post_worker = {});
  ~DhtServer();

  DhtServer(const DhtServer&) = delete;
  DhtServer& operator=(const DhtServer&) = delete;

  /** Uses local_peer_id and the inbound rate limits. */
  void Configure(const AmpDhtProtocolConfig& config);
  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  Stats InboundStats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
  bool started_ = false;
};

} // namespace pbr
