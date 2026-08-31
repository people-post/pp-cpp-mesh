#include "base/p2p/AmpCircuitHopRegistry.h"

namespace pbr {

std::string AmpCircuitHopRegistry::Key(const std::string& peer_key, const std::string& target_protocol) {
  return peer_key + '\x1f' + target_protocol;
}

Roe<void> AmpCircuitHopRegistry::Install(const std::string& peer_key, const std::string& relay_peer_key,
                                         const std::string& target_protocol,
                                         std::shared_ptr<amp::ChannelSession> session,
                                         const CircuitTunnelId tunnel_id) {
  if (peer_key.empty() || relay_peer_key.empty() || target_protocol.empty() || !session) {
    return Error("amp circuit hop install incomplete");
  }
  Hop hop;
  hop.session = std::move(session);
  hop.relay_peer_key = relay_peer_key;
  hop.target_protocol = target_protocol;
  hop.tunnel_id = tunnel_id;
  std::lock_guard lock(mu_);
  hops_[Key(peer_key, target_protocol)] = std::move(hop);
  return {};
}

std::optional<AmpCircuitHopRegistry::Hop> AmpCircuitHopRegistry::Find(
    const std::string& peer_key, const std::string& target_protocol) const {
  std::lock_guard lock(mu_);
  const auto it = hops_.find(Key(peer_key, target_protocol));
  if (it == hops_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool AmpCircuitHopRegistry::HasAny(const std::string& peer_key) const {
  if (peer_key.empty()) {
    return false;
  }
  std::lock_guard lock(mu_);
  for (const auto& [key, _] : hops_) {
    if (key.size() >= peer_key.size() + 1 && key.compare(0, peer_key.size(), peer_key) == 0 &&
        key[peer_key.size()] == '\x1f') {
      return true;
    }
  }
  return false;
}

void AmpCircuitHopRegistry::Clear(const std::string& peer_key) {
  if (peer_key.empty()) {
    return;
  }
  std::lock_guard lock(mu_);
  for (auto it = hops_.begin(); it != hops_.end();) {
    if (it->first.size() >= peer_key.size() + 1 && it->first.compare(0, peer_key.size(), peer_key) == 0 &&
        it->first[peer_key.size()] == '\x1f') {
      it = hops_.erase(it);
    } else {
      ++it;
    }
  }
}

void AmpCircuitHopRegistry::Clear(const std::string& peer_key, const std::string& target_protocol) {
  std::lock_guard lock(mu_);
  hops_.erase(Key(peer_key, target_protocol));
}

void AmpCircuitHopRegistry::ClearAll() {
  std::lock_guard lock(mu_);
  hops_.clear();
}

} // namespace pbr
