#pragma once

#include "amp/link/MeshRuntime.h"
#include "mesh/dht/DhtRateLimiter.h"
#include "mesh/dht/DhtRecordStore.h"
#include "mesh/dht/DhtRoutingTable.h"
#include "mesh/dht/DhtTypes.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pp::mesh {

/**
 * Serving side of the Amp mesh DHT (`/pp-mesh/dht/1.0.0`): answers `ping`, `find_peer` from the
 * shared record store and accepts signed self `store`s (the record must be the sender's own,
 * unexpired, not a seq regression), rate limited per remote peer.
 *
 * `find_peer` also offers up to `kDhtCloserPeers` contacts from the routing table closest to the
 * target (never the asker). A peer whose `store` is accepted participates: `note_heard` gets it
 * with the dial address from its record (docs/DHT.md § 4.2 admission).
 */
class DhtServer {
public:
  using WorkerPost = std::function<void(std::function<void()>)>;
  /** A participant was heard from: (peer_id, dial multiaddr). */
  using NoteHeard = std::function<void(const std::string& peer_id, const std::string& dial_addr)>;

  struct Stats {
    uint64_t inbound_find_peer = 0;
    uint64_t inbound_store = 0;
    uint64_t inbound_rate_limited = 0;
    uint64_t store_rejected = 0;
  };

  DhtServer(pp::amp::MeshRuntime& runtime, DhtRecordStore& store, const DhtRoutingTable& table,
            NoteHeard note_heard, WorkerPost post_worker = {});
  ~DhtServer();

  DhtServer(const DhtServer&) = delete;
  DhtServer& operator=(const DhtServer&) = delete;

  /** Uses local_peer_id, participate and the inbound rate limits. */
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

} // namespace pp::mesh
