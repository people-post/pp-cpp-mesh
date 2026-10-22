#include "mesh/dht/AmpDhtProtocol.h"

#include "common/ValueJson.h"
namespace pp::mesh {

AmpDhtProtocol::AmpDhtProtocol(pp::amp::MeshRuntime& runtime, WorkerPost post_worker)
    : server_(runtime, store_, table_,
              [this](const std::string& peer_id, const std::string& dial_addr) { client_.NoteHeard(peer_id, dial_addr); },
              std::move(post_worker)),
      client_(runtime, store_, table_) {}

void AmpDhtProtocol::Configure(AmpDhtProtocolConfig config) {
  config_ = std::move(config);
  table_.Reset(config_.local_peer_id,
               config_.tunables.k_bucket_size > 0 ? static_cast<size_t>(config_.tunables.k_bucket_size) : 0);
  server_.Configure(config_);
  client_.Configure(config_);
}

void AmpDhtProtocol::Start() {
  if (client_.IsStarted()) {
    return;
  }
  server_.Start();
  client_.Start();
  if (config_.participate) {
    Tick();
  }
}

void AmpDhtProtocol::Stop() {
  client_.Stop();
  server_.Stop();
}

DhtOpsStats AmpDhtProtocol::Stats() const {
  DhtOpsStats stats;
  stats.started = IsStarted();
  stats.participate = config_.participate;
  stats.cached_records = store_.Size();
  stats.routing_contacts = table_.Size();
  const DhtServer::Stats inbound = server_.InboundStats();
  stats.inbound_find_peer = inbound.inbound_find_peer;
  stats.inbound_store = inbound.inbound_store;
  stats.inbound_rate_limited = inbound.inbound_rate_limited;
  stats.store_rejected = inbound.store_rejected;
  const DhtClient::Stats outbound = client_.OutboundStats();
  stats.find_peer_issued = outbound.find_peer_issued;
  stats.self_publishes = outbound.self_publishes;
  stats.last_publish_targets = outbound.last_publish_targets;
  stats.soft_reputation_skips = outbound.soft_reputation_skips;
  stats.soft_reputation_penalized_peers = outbound.soft_reputation_penalized_peers;
  return stats;
}

std::string AmpDhtProtocol::FormatOpsStatusJson() const {
  const DhtOpsStats stats = Stats();
  common::Object object;
  object.set("started", stats.started);
  object.set("participate", stats.participate);
  object.set("cached_records", static_cast<int64_t>(stats.cached_records));
  object.set("routing_contacts", static_cast<int64_t>(stats.routing_contacts));
  object.set("inbound_find_peer", static_cast<int64_t>(stats.inbound_find_peer));
  object.set("inbound_store", static_cast<int64_t>(stats.inbound_store));
  object.set("inbound_rate_limited", static_cast<int64_t>(stats.inbound_rate_limited));
  object.set("store_rejected", static_cast<int64_t>(stats.store_rejected));
  object.set("find_peer_issued", static_cast<int64_t>(stats.find_peer_issued));
  object.set("self_publishes", static_cast<int64_t>(stats.self_publishes));
  object.set("last_publish_targets", static_cast<int64_t>(stats.last_publish_targets));
  object.set("soft_reputation_skips", static_cast<int64_t>(stats.soft_reputation_skips));
  object.set("soft_reputation_penalized_peers",
             static_cast<int64_t>(stats.soft_reputation_penalized_peers));
  std::vector<common::Value> record_rows;
  for (const PeerRoutingRecord& record : SnapshotRecords()) {
    common::Object row;
    row.set("peer_id", record.peer_id);
    row.set("seq", record.seq);
    std::vector<common::Value> addrs;
    for (const std::string& ma : record.multiaddrs) {
      addrs.emplace_back(ma);
    }
    row.set("multiaddrs", common::makeArray(std::move(addrs)));
    if (record.capabilities) {
      common::Object caps;
      caps.set("circuit_relay", record.capabilities->circuit_relay);
      caps.set("media_relay", record.capabilities->media_relay);
      row.set("capabilities", std::move(caps));
    }
    record_rows.emplace_back(std::make_shared<common::Object>(std::move(row)));
  }
  object.set("records", common::makeArray(std::move(record_rows)));
  return common::DumpJson(object);
}

} // namespace pp::mesh
