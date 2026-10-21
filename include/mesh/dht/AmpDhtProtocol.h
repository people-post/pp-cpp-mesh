#pragma once

#include "mesh/dht/DhtRecordStore.h"
#include "mesh/dht/DhtRoutingTable.h"
#include "mesh/dht/DhtTypes.h"
#include "mesh/dht/client/DhtClient.h"
#include "mesh/dht/serve/DhtServer.h"

#include "amp/link/MeshRuntime.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pp::mesh {

/**
 * Amp L4 mesh DHT (`/pp-mesh/dht/1.0.0`) — FIND_PEER + self STORE. Owns the record store both
 * ends share: `serve/DhtServer` answers find_peer / store from it, `client/DhtClient` looks peers
 * up (caching what it finds) and publishes the self record while participating. MeshHost owns one
 * when Amp is up.
 */
class AmpDhtProtocol {
public:
  using Err = DhtClient::Err;
  using Failure = DhtClient::Failure;
  using FindPeerRoe = DhtClient::FindPeerRoe;
  using RpcRoe = DhtClient::RpcRoe;
  using WorkerPost = DhtServer::WorkerPost;

  /** Map immediate link-manager failure → DHT Err (never inspect ADP/PeerLink codes). */
  static Failure WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child) {
    return DhtClient::WrapLinkFailure(child);
  }

  AmpDhtProtocol(pp::amp::MeshRuntime& runtime, WorkerPost post_worker = {});

  AmpDhtProtocol(const AmpDhtProtocol&) = delete;
  AmpDhtProtocol& operator=(const AmpDhtProtocol&) = delete;

  void Configure(AmpDhtProtocolConfig config);
  void Start();
  void Stop();
  bool IsStarted() const { return client_.IsStarted(); }

  /** Periodic, on the io lane: publish the self record when due, refresh stale buckets, and drop
   * contacts the reach record holds down. */
  void Tick() {
    client_.PublishSelfIfDue();
    client_.RefreshIfDue();
    client_.SweepIfDue();
  }

  void FindPeer(const std::string& target_peer_id, std::function<void(FindPeerRoe)> on_done) {
    client_.FindPeer(target_peer_id, std::move(on_done));
  }

  /** Drop routing contacts the reach record holds down now (Tick does it every 30 s). */
  void SweepHeldDown() { client_.SweepHeldDown(); }

  std::optional<PeerRoutingRecord> LocalRecord(const std::string& peer_id) const { return store_.Get(peer_id); }
  const DhtRoutingTable& RoutingTable() const { return table_; }
  std::vector<PeerRoutingRecord> SnapshotRecords() const { return store_.Snapshot(); }
  DhtOpsStats Stats() const;
  std::string FormatOpsStatusJson() const;

private:
  DhtRecordStore store_;
  DhtRoutingTable table_;
  AmpDhtProtocolConfig config_;
  DhtServer server_;
  DhtClient client_;
};

} // namespace pp::mesh
