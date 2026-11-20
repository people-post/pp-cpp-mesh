#include "mesh/dht/serve/DhtServer.h"

#include "mesh/l4/shared/InboundReply.h"
#include "mesh/l4/shared/WireVersion.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "mesh/dht/DhtKey.h"
#include "mesh/dht/DhtRecordCodec.h"
#include "common/ValueJson.h"
#include "crypto/AlgId.h"
#include "crypto/SodiumUtil.h"
#include "common/Lifetime.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <mutex>
namespace pp::mesh {
namespace {

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const DhtServer::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

common::Object MakeErrorResponse(const std::string& req_id, const std::string& code, const std::string& message) {
  common::Object object;
  object.set("op", "error");
  object.set("req_id", req_id);
  object.set("version", int64_t{kDhtWireVersion});
  object.set("code", code);
  object.set("message", message);
  return object;
}

/** A record's `public_key_b64` names the sender's key: the same typed blob, whatever its base64 spelling. */
bool SameTypedKey(const std::string& record_key_b64, const std::string& own_key_b64) {
  auto record_key = pp::Base64Decode(record_key_b64);
  auto own_key = pp::Base64Decode(own_key_b64);
  return record_key && own_key && *record_key == *own_key;
}

/** `signature_algs` on a record's `bad_version`: the names of the signature algorithms this build verifies. */
common::Value SignatureAlgNames() {
  const pp::AlgInfo* alg = pp::FindAlg(pp::AlgKind::kSignature, pp::kSignatureAlgMlDsa65);
  return common::makeArray({common::Value(std::string(alg ? alg->name : "ml-dsa-65"))});
}

common::Value RecordsToJsonArray(const std::vector<PeerRoutingRecord>& records) {
  std::vector<common::Value> elements;
  elements.reserve(records.size());
  for (const PeerRoutingRecord& record : records) {
    elements.emplace_back(std::make_shared<common::Object>(PeerRoutingRecordToObject(record)));
  }
  return common::makeArray(std::move(elements));
}

} // namespace

struct DhtServer::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  DhtRecordStore* store = nullptr;
  const DhtRoutingTable* table = nullptr;
  NoteHeard note_heard;
  WorkerPost post_worker;
  /** Answers say whether we take part (a peer is admitted from its answers only if it does). */
  std::atomic<bool> participate{false};
  std::atomic<bool> stopped{false};
  /** Guards protocol-handler raw Impl* past Stop — OWNERSHIP.md § Lifetime. */
  pp::Lifetime deferred;
  std::string local_peer_id;
  DhtRateLimiter limiter;
  mutable std::mutex stats_mutex;
  Stats stats;

  pp::amp::PeerLinkManager& Links() { return runtime->Links(); }
  /** IO lane for InboundReply. */
  InboundReply::IoPost IoPost() {
    return [rt = runtime](std::function<void()> task) { rt->PostToIo(std::move(task)); };
  }

  void HandleInboundOnLink(pp::amp::LinkHandle handle, const std::string& remote_peer_id, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime || remote_peer_id.empty()) {
      return;
    }
    pp::amp::ByteVector remote_pk;
    Links().WithLiveLink(handle, [&](pp::amp::PeerLink& link) { remote_pk = link.RemoteIdentityPublicKey(); });
    auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    *session_holder = Links().BindChannelOnLink(
        handle, channel_id, InboundReplyPolicy(pp::amp::ControlJsonChannelPolicy()),
        [this, session_holder, remote_pk = std::move(remote_pk),
         remote_peer = remote_peer_id](Roe<std::vector<uint8_t>> frame) mutable {
          auto session = *session_holder;
          if (!session || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          auto body = std::move(*frame);
          // Keep the channel open for the worker's reply (InboundReply.h); `reply` closes it.
          auto reply = MakeInboundReply(session, IoPost());
          RunWorker(post_worker, [this, reply, body = std::move(body), remote_pk = std::move(remote_pk),
                                  remote_peer = std::move(remote_peer)]() mutable {
            if (stopped.load(std::memory_order_acquire)) {
              return;
            }
            const std::string json_utf8(body.begin(), body.end());
            auto root = common::TryParseObject(json_utf8);
            if (!root) {
              common::Object resp = MakeErrorResponse("", "invalid_json", "invalid json");
              reply->Send(JsonToBody(common::DumpJson(resp)));
              reply->Close();
              return;
            }
            const std::string req_id = root->getString("req_id").value_or("");
            // Before the op and any crypto. Strict, as in v9.0.0 (a missing version is refused too); the refusal now
            // names the versions this node reads (U7).
            const int version = static_cast<int>(root->getIf<int64_t>("version").value_or(0));
            if (version != kDhtWireVersion) {
              common::Object resp =
                  MakeErrorResponse(req_id, kBadVersionCode, "unsupported dht version " + std::to_string(version));
              SetSupportedVersions(resp, {kDhtWireVersion});
              reply->Send(JsonToBody(common::DumpJson(resp)));
              reply->Close();
              return;
            }
            const std::string op = root->getString("op").value_or("");
            common::Object response;
            if (op == "ping") {
              response.set("op", "pong");
              response.set("req_id", req_id);
              response.set("version", int64_t{kDhtWireVersion});
              response.set("peer_id", local_peer_id);
              response.set("participant", participate.load(std::memory_order_acquire));
            } else if (op == "find_peer" || op == "store") {
              if (!limiter.Allow(remote_peer)) {
                {
                  std::lock_guard lock(stats_mutex);
                  ++stats.inbound_rate_limited;
                }
                response = MakeErrorResponse(req_id, "rate_limited", "dht rate limited");
              } else if (op == "find_peer") {
                {
                  std::lock_guard lock(stats_mutex);
                  ++stats.inbound_find_peer;
                }
                response.set("op", "find_peer_result");
                response.set("req_id", req_id);
                response.set("version", int64_t{kDhtWireVersion});
                const std::string peer_id = root->getString("peer_id").value_or("");
                response.set("peer_id", peer_id);
                std::vector<PeerRoutingRecord> records;
                if (auto local = store->Get(peer_id)) {
                  records.push_back(*local);
                }
                response.set("records", RecordsToJsonArray(records));
                std::vector<DhtCloserPeer> closer;
                if (const auto target_key = DhtKeyOfPeerId(peer_id)) {
                  const size_t n = std::min(table->K(), kDhtCloserPeers); // k_closer = k
                  for (DhtContact& contact : table->Closest(*target_key, n, remote_peer)) {
                    closer.push_back({std::move(contact.peer_id), std::move(contact.multiaddr)});
                  }
                }
                response.set("closer_peers", DhtCloserPeersToArray(closer));
                response.set("participant", participate.load(std::memory_order_acquire));
              } else {
                {
                  std::lock_guard lock(stats_mutex);
                  ++stats.inbound_store;
                }
                std::string reject_code;
                std::string reject_text;
                bool ok = false;
                const common::Object* record_obj = root->getObject("record");
                if (!record_obj) {
                  reject_code = "missing_record";
                } else if (auto parsed = PeerRoutingRecordFromObject(*record_obj); !parsed) {
                  // A record this build cannot read (V3): a typed refusal, before the signature check — a v1 record
                  // (older, below the floor), a newer version, or an algorithm this build does not know.
                  const PeerRoutingRecordErr err = parsed.error().GetCode();
                  if (err == PeerRoutingRecordErr::Malformed) {
                    reject_code = "invalid_record";
                  } else {
                    reject_code = kBadVersionCode;
                  }
                  reject_text = parsed.error().message;
                } else if (parsed->peer_id != remote_peer) {
                  reject_code = "not_self";
                } else if (PeerRoutingRecordExpired(*parsed, static_cast<int64_t>(std::time(nullptr)))) {
                  reject_code = "expired";
                } else if (auto verified = VerifyPeerRoutingRecord(*parsed, remote_pk); !verified) {
                  if (verified.error().GetCode() == PeerRoutingRecordErr::BadSignature) {
                    reject_code = "bad_signature";
                  } else {
                    reject_code = "invalid_record";
                  }
                } else if (PeerRoutingRecordHasUnknownCritical(*parsed)) {
                  // It verifies, but says this node must understand something it does not: update needed.
                  reject_code = kBadVersionCode;
                  reject_text = "unknown critical extension";
                } else if (auto own_key = PeerRoutingPublicKeyB64(remote_pk); !own_key) {
                  reject_code = "bad_key";
                } else if (!parsed->public_key_b64.empty() && !SameTypedKey(parsed->public_key_b64, *own_key)) {
                  reject_code = "bad_key";
                } else if (auto existing = store->Get(parsed->peer_id); existing && existing->seq > parsed->seq) {
                  reject_code = "seq_regression";
                } else if (parsed->public_key_b64 = *own_key; !store->Put(*parsed)) { // served checkable to anyone
                  reject_code = "store_failed";
                } else {
                  ok = true;
                }
                if (!ok) {
                  std::lock_guard lock(stats_mutex);
                  ++stats.store_rejected;
                } else if (note_heard) {
                  // It publishes, so it takes part; its own record says where to dial it.
                  if (auto stored = store->Get(remote_peer)) {
                    note_heard(remote_peer, PeerRoutingRecordDialAddr(*stored));
                  }
                }
                if (!ok && !reject_code.empty()) {
                  response = MakeErrorResponse(req_id, reject_code, reject_text.empty() ? reject_code : reject_text);
                  if (reject_code == kBadVersionCode) {
                    SetSupportedVersions(response, {kPeerRoutingRecordVersion}, "record_versions");
                    response.set("signature_algs", SignatureAlgNames());
                  }
                } else {
                  response.set("op", "store_result");
                  response.set("req_id", req_id);
                  response.set("version", int64_t{kDhtWireVersion});
                  response.set("ok", ok);
                }
              }
            } else {
              response = MakeErrorResponse(req_id, "unsupported_op", "unsupported op");
            }
            reply->Send(JsonToBody(common::DumpJson(response)));
            reply->Close();
          });
          return true;
        });
  }
};

DhtServer::DhtServer(pp::amp::MeshRuntime& runtime, DhtRecordStore& store, const DhtRoutingTable& table,
                     NoteHeard note_heard, WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
  impl_->store = &store;
  impl_->table = &table;
  impl_->note_heard = std::move(note_heard);
  impl_->post_worker = std::move(post_worker);
}

DhtServer::~DhtServer() {
  Stop();
}

void DhtServer::Configure(const AmpDhtProtocolConfig& config) {
  impl_->local_peer_id = config.local_peer_id;
  impl_->participate.store(config.participate, std::memory_order_release);
  impl_->limiter.Configure(config.tunables.inbound_ops_per_peer_per_window,
                           config.tunables.inbound_rate_window_seconds);
}

void DhtServer::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  runtime_.Links().SetProtocolHandler(
      kDhtProtocolId,
      impl_->deferred.Bind([impl = impl_.get()](pp::amp::LinkHandle handle, const std::string& remote_peer_id,
                                                const uint32_t channel_id) {
        impl->HandleInboundOnLink(handle, remote_peer_id, channel_id);
      }));
}

void DhtServer::Stop() {
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.Links().RemoveProtocolHandler(kDhtProtocolId);
  impl_->deferred.Invalidate();
}

DhtServer::Stats DhtServer::InboundStats() const {
  std::lock_guard lock(impl_->stats_mutex);
  return impl_->stats;
}

} // namespace pp::mesh
