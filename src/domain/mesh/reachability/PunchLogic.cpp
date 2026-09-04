#include "domain/mesh/reachability/PunchLogic.h"

#include "amp/link/AdpMultiaddr.h"
#include "common/PbrCompat.h"

namespace pbr {
namespace {

Value AddrsArray(const std::vector<std::string>& addrs) {
  std::vector<Value> items;
  items.reserve(addrs.size());
  for (const std::string& ma : addrs) {
    items.emplace_back(ma);
  }
  return ArrayValue(std::move(items));
}

std::vector<std::string> ReadAddrs(const Object& root, const char* key) {
  std::vector<std::string> out;
  if (const Array* arr = root.getArray(key)) {
    for (const auto& item : arr->elements) {
      if (auto s = asString(item)) {
        out.push_back(*s);
      }
    }
  }
  return out;
}

} // namespace

std::string EncodePunchConnect(const PunchConnectRequest& req) {
  Object o;
  o.set("v", int64_t{1});
  o.set("op", "connect");
  o.set("target_peer_id", req.target_peer_id);
  o.set("addrs", AddrsArray(req.addrs));
  o.set("window_ms", int64_t{req.window_ms > 0 ? req.window_ms : 2000});
  o.set("reason", req.reason.empty() ? "cold" : req.reason);
  return DumpJson(o);
}

std::string EncodePunchOffer(const PunchOffer& msg) {
  Object o;
  o.set("v", int64_t{1});
  o.set("op", "offer");
  o.set("initiator_peer_id", msg.initiator_peer_id);
  o.set("addrs", AddrsArray(msg.addrs));
  o.set("epoch_id", msg.epoch_id);
  o.set("window_ms", int64_t{msg.window_ms > 0 ? msg.window_ms : 2000});
  return DumpJson(o);
}

std::string EncodePunchCandidates(const PunchCandidates& msg) {
  Object o;
  o.set("v", int64_t{1});
  o.set("op", "candidates");
  o.set("peer_id", msg.peer_id);
  o.set("addrs", AddrsArray(msg.addrs));
  o.set("nonce", msg.nonce);
  return DumpJson(o);
}

std::string EncodePunchSync(const PunchSync& msg) {
  Object o;
  o.set("v", int64_t{1});
  o.set("op", "sync");
  o.set("epoch_id", msg.epoch_id);
  o.set("peer_addrs", AddrsArray(msg.peer_addrs));
  o.set("window_ms", int64_t{msg.window_ms > 0 ? msg.window_ms : 2000});
  return DumpJson(o);
}

std::string EncodePunchResult(const PunchResult& msg) {
  Object o;
  o.set("v", int64_t{1});
  o.set("op", "result");
  o.set("epoch_id", msg.epoch_id);
  o.set("ok", msg.ok);
  o.set("winner_multiaddr", msg.winner_multiaddr);
  o.set("error", msg.error);
  return DumpJson(o);
}

std::optional<std::string> PunchOp(const Object& root) { return root.getString("op"); }

std::optional<PunchConnectRequest> DecodePunchConnect(const Object& root) {
  if (root.getString("op").value_or("") != "connect") {
    return std::nullopt;
  }
  PunchConnectRequest req;
  req.target_peer_id = root.getString("target_peer_id").value_or("");
  req.addrs = SanitizePunchAddrs(ReadAddrs(root, "addrs"));
  req.window_ms = static_cast<int>(root.getNonNegInt("window_ms").value_or(2000));
  req.reason = root.getString("reason").value_or("cold");
  if (req.target_peer_id.empty()) {
    return std::nullopt;
  }
  return req;
}

std::optional<PunchOffer> DecodePunchOffer(const Object& root) {
  if (root.getString("op").value_or("") != "offer") {
    return std::nullopt;
  }
  PunchOffer msg;
  msg.initiator_peer_id = root.getString("initiator_peer_id").value_or("");
  msg.addrs = SanitizePunchAddrs(ReadAddrs(root, "addrs"));
  msg.epoch_id = root.getString("epoch_id").value_or("");
  msg.window_ms = static_cast<int>(root.getNonNegInt("window_ms").value_or(2000));
  if (msg.initiator_peer_id.empty() || msg.epoch_id.empty()) {
    return std::nullopt;
  }
  return msg;
}

std::optional<PunchCandidates> DecodePunchCandidates(const Object& root) {
  if (root.getString("op").value_or("") != "candidates") {
    return std::nullopt;
  }
  PunchCandidates msg;
  msg.peer_id = root.getString("peer_id").value_or("");
  msg.addrs = SanitizePunchAddrs(ReadAddrs(root, "addrs"));
  msg.nonce = root.getString("nonce").value_or("");
  return msg;
}

std::optional<PunchSync> DecodePunchSync(const Object& root) {
  if (root.getString("op").value_or("") != "sync") {
    return std::nullopt;
  }
  PunchSync msg;
  msg.epoch_id = root.getString("epoch_id").value_or("");
  msg.peer_addrs = SanitizePunchAddrs(ReadAddrs(root, "peer_addrs"));
  msg.window_ms = static_cast<int>(root.getNonNegInt("window_ms").value_or(2000));
  if (msg.epoch_id.empty()) {
    return std::nullopt;
  }
  return msg;
}

std::optional<PunchResult> DecodePunchResult(const Object& root) {
  if (root.getString("op").value_or("") != "result") {
    return std::nullopt;
  }
  PunchResult msg;
  msg.epoch_id = root.getString("epoch_id").value_or("");
  msg.ok = root.getIf<bool>("ok").value_or(false);
  msg.winner_multiaddr = root.getString("winner_multiaddr").value_or("");
  msg.error = root.getString("error").value_or("");
  return msg;
}

std::vector<std::string> SanitizePunchAddrs(const std::vector<std::string>& addrs, size_t max_addrs) {
  std::vector<std::string> out;
  out.reserve(std::min(addrs.size(), max_addrs));
  for (const std::string& ma : addrs) {
    if (out.size() >= max_addrs) {
      break;
    }
    if (ma.empty() || !pp::amp::ParseAdpMultiaddr(ma)) {
      continue;
    }
    bool dup = false;
    for (const std::string& existing : out) {
      if (existing == ma) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      out.push_back(ma);
    }
  }
  return out;
}

bool PunchWindowOpen(int64_t start_ms, int window_ms, int64_t now_ms) {
  if (window_ms <= 0) {
    return false;
  }
  return now_ms >= start_ms && (now_ms - start_ms) <= static_cast<int64_t>(window_ms);
}

} // namespace pbr
