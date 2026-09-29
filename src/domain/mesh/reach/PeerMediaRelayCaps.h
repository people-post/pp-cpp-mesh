#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace pbr {

/**
 * Which mesh peers (by PeerId) advertised serving media_relay — learned from their caps ads (invite /
 * accept / caps update). Not thread-safe: one owner thread.
 */
class PeerMediaRelayCaps {
public:
  /** `peer_id` advertised `media_relay`; true when it is newly known as capable. */
  bool Note(const std::string& peer_id, bool media_relay) {
    if (peer_id.empty()) {
      return false;
    }
    const bool was = Has(peer_id);
    caps_[peer_id] = media_relay;
    return media_relay && !was;
  }
  bool Has(const std::string& peer_id) const {
    const auto it = caps_.find(peer_id);
    return it != caps_.end() && it->second;
  }
  std::vector<std::string> ListCapable() const {
    std::vector<std::string> out;
    out.reserve(caps_.size());
    for (const auto& [peer_id, capable] : caps_) {
      if (capable && !peer_id.empty()) {
        out.push_back(peer_id);
      }
    }
    return out;
  }

private:
  std::unordered_map<std::string, bool> caps_;
};

} // namespace pbr
