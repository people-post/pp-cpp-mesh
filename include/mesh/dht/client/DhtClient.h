#pragma once

#include "amp/link/MeshRuntime.h"
#include "mesh/dht/DhtRecordStore.h"
#include "mesh/dht/DhtRoutingTable.h"
#include "mesh/dht/DhtTypes.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/ValueJson.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pp::mesh {

/**
 * Client side of the Amp mesh DHT (`/pp-mesh/dht/1.0.0`): FIND_PEER fanned out to the query peers
 * (thin bootstrap, soft reputation, concurrency cap) and, when participating, the periodic signed
 * self record pushed to them. Found records land in the shared record store.
 *
 * It keeps the routing table: a peer that answers as a participant, or whose `store` the server
 * accepted, is `NoteHeard`; when its bucket is full and the bucket's oldest has been quiet a while,
 * the oldest is pinged and leaves if it does not answer (docs/DHT.md § 4.2, D5).
 *
 * Errors follow pp-browser docs/contracts/CODED_FAILURE.md — wrap PeerLinkManager failures at this owning layer.
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
  using RpcRoe = CodedRoe<common::Object, Err>;

  struct Stats {
    uint64_t find_peer_issued = 0;
    uint64_t soft_reputation_skips = 0;
    uint64_t soft_reputation_penalized_peers = 0;
  };

  /** Map immediate link-manager failure → DHT Err (never inspect ADP/PeerLink codes). */
  static Failure WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child);

  /** A full bucket's oldest heard from within this long stays without a ping. */
  static constexpr int64_t kQuietBeforePingMs = 15 * 60 * 1000;

  DhtClient(pp::amp::MeshRuntime& runtime, DhtRecordStore& store, DhtRoutingTable& table);

  DhtClient(const DhtClient&) = delete;
  DhtClient& operator=(const DhtClient&) = delete;

  void Configure(const AmpDhtProtocolConfig& config);
  void Start();
  void Stop();
  bool IsStarted() const { return started_.load(std::memory_order_acquire); }

  /** Participating: refresh the self record when due and push it to the query peers. */
  void PublishSelfIfDue();
  void FindPeer(const std::string& target_peer_id, std::function<void(FindPeerRoe)> on_done);
  /** A DHT participant was heard from over an authenticated exchange: admit or refresh it. */
  void NoteHeard(const std::string& peer_id, const std::string& dial_addr);

  Stats OutboundStats() const;

private:
  void Rpc(const std::string& peer_key, common::Object request, std::function<void(RpcRoe)> on_response);
  void NoteSoftReputationBad(const std::string& peer_key);
  bool SoftReputationAllows(const std::string& peer_key) const;
  std::vector<std::string> FilteredQueryPeerKeys();
  /** Ping a full bucket's oldest: it stays if it answers, else it leaves for a replacement. */
  void PingContact(const DhtContact& contact);
  /** The answering peer took part: admit it with the dial address from its own record. */
  void NoteAnswered(const std::string& peer_key, const common::Object& response,
                    const std::optional<PeerRoutingRecord>& its_record);

  pp::amp::MeshRuntime& runtime_;
  DhtRecordStore& store_;
  DhtRoutingTable& table_;
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

  std::mutex ping_mutex_;
  std::unordered_set<std::string> pinging_;
};

} // namespace pp::mesh
