#pragma once

#include "domain/mesh/dht/DhtRecordStore.h"
#include "domain/mesh/dht/DhtTypes.h"
#include "domain/mesh/dht/client/DhtClient.h"
#include "domain/mesh/dht/serve/DhtServer.h"

#include "amp/link/MeshRuntime.h"
#include "common/PbrCompat.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

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

  /** Periodic: refresh self record + push to bootstrap peers. */
  void Tick() { client_.PublishSelfIfDue(); }

  void FindPeer(const std::string& target_peer_id, std::function<void(FindPeerRoe)> on_done) {
    client_.FindPeer(target_peer_id, std::move(on_done));
  }

  std::optional<PeerRoutingRecord> LocalRecord(const std::string& peer_id) const { return store_.Get(peer_id); }
  std::vector<PeerRoutingRecord> SnapshotRecords() const { return store_.Snapshot(); }
  DhtOpsStats Stats() const;
  std::string FormatOpsStatusJson() const;

private:
  DhtRecordStore store_;
  AmpDhtProtocolConfig config_;
  DhtServer server_;
  DhtClient client_;
};

} // namespace pbr
