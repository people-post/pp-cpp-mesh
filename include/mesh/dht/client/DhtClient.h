#pragma once

#include "amp/link/MeshRuntime.h"
#include "mesh/dht/DhtRecordStore.h"
#include "mesh/dht/DhtRoutingTable.h"
#include "mesh/dht/client/DhtLookup.h"
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
 * Client side of the Amp mesh DHT (`/pp-mesh/dht/1.0.0`): FIND_PEER as an iterative lookup
 * (`DhtLookup`, docs/DHT.md § 4.4) from the query peers and — taking part — the routing table's
 * closest, following `closer_peers` hints; a Client asks its query peers only (D2). Soft
 * reputation, a concurrency cap and a 60 s negative cache bound it. Taking part, it also pushes the
 * periodic signed self record. Found records land in the shared record store.
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

  /** A whole lookup stops asking after this long (the asks in flight still settle). */
  static constexpr int64_t kLookupBudgetMs = 8000;
  /** A lookup that someone answered and nobody had a record for is not repeated this soon. */
  static constexpr int64_t kNotFoundCacheMs = 60 * 1000;
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
  /** One `find_peer` to `candidate` within a lookup; the answer's records already checked. */
  void AskForPeer(const DhtLookupCandidate& candidate, const std::string& target_peer_id,
                  std::function<void(CodedRoe<DhtLookupAnswer, Err>)> done);
  /** Ping a full bucket's oldest: it stays if it answers, else it leaves for a replacement. */
  void PingContact(const DhtContact& contact);
  /**
   * The answering peer took part: admit it with the dial address from its own record, else the
   * address we just reached it at (`reached_at`, a hint the authenticated dial proved).
   */
  void NoteAnswered(const std::string& peer_key, const common::Object& response,
                    const std::optional<PeerRoutingRecord>& its_record, const std::string& reached_at);

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

  std::mutex not_found_mutex_;
  std::unordered_map<std::string, int64_t> not_found_until_ms_;

  std::mutex ping_mutex_;
  std::unordered_set<std::string> pinging_;
};

} // namespace pp::mesh
