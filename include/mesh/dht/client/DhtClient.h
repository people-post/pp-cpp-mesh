#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/dht/DhtRecordStore.h"
#include "domain/mesh/dht/DhtTypes.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pbr {

/**
 * Client side of the Amp mesh DHT (`/pp-mesh/dht/1.0.0`): FIND_PEER fanned out to the query peers
 * (thin bootstrap, soft reputation, concurrency cap) and, when participating, the periodic signed
 * self record pushed to them. Found records land in the shared record store.
 *
 * Errors follow docs/contracts/CODED_FAILURE.md — wrap PeerLinkManager failures at this owning layer.
 */
class DhtClient {
public:
  enum class Err : int32_t {
    Ok = 0,
    NotStarted,
    EndpointNotRegistered,
    InvalidRequest,
    LinkFailed,
    Timeout,
    ChannelFailed,
    ProtocolError,
    ConcurrencyLimit,
    NotFound,
    Generic,
  };

  using Failure = CodedFailure<Err>;
  using FindPeerRoe = CodedRoe<DhtFindPeerResult, Err>;
  using RpcRoe = CodedRoe<Object, Err>;

  struct Stats {
    uint64_t find_peer_issued = 0;
    uint64_t soft_reputation_skips = 0;
    uint64_t soft_reputation_penalized_peers = 0;
  };

  /** Map immediate link-manager failure → DHT Err (never inspect ADP/PeerLink codes). */
  static Failure WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child);

  DhtClient(pp::amp::MeshRuntime& runtime, DhtRecordStore& store);

  DhtClient(const DhtClient&) = delete;
  DhtClient& operator=(const DhtClient&) = delete;

  void Configure(const AmpDhtProtocolConfig& config);
  void Start();
  void Stop();
  bool IsStarted() const { return started_.load(std::memory_order_acquire); }

  /** Participating: refresh the self record when due and push it to the query peers. */
  void PublishSelfIfDue();
  void FindPeer(const std::string& target_peer_id, std::function<void(FindPeerRoe)> on_done);

  Stats OutboundStats() const;

private:
  void Rpc(const std::string& peer_key, Object request, std::function<void(RpcRoe)> on_response);
  void NoteSoftReputationBad(const std::string& peer_key);
  bool SoftReputationAllows(const std::string& peer_key) const;
  std::vector<std::string> FilteredQueryPeerKeys();

  pp::amp::MeshRuntime& runtime_;
  DhtRecordStore& store_;
  AmpDhtProtocolConfig config_;
  std::atomic<bool> started_{false};
  int64_t self_seq_ = 0;
  std::chrono::steady_clock::time_point next_self_publish_{};

  mutable std::mutex stats_mutex_;
  uint64_t find_peer_issued_ = 0;
  uint64_t soft_reputation_skips_ = 0;

  mutable std::mutex reputation_mutex_;
  struct SoftRep {
    int bad_count = 0;
    std::chrono::steady_clock::time_point cooldown_until{};
  };
  std::unordered_map<std::string, SoftRep> soft_reputation_;

  std::atomic<int> inflight_lookups_{0};
};

} // namespace pbr
