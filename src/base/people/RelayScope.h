#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

namespace pbr {

/** Relay scope bands for escalate pick (N023). Bit flags. */
enum class RelayScope : uint8_t {
  Link = 1 << 0,
  Site = 1 << 1,
  Social = 1 << 2,
  Org = 1 << 3,
  Public = 1 << 4,
};

using RelayScopeMask = uint8_t;

inline constexpr RelayScopeMask kRelayScopeLinkSiteSocial =
    static_cast<RelayScopeMask>(RelayScope::Link) | static_cast<RelayScopeMask>(RelayScope::Site) |
    static_cast<RelayScopeMask>(RelayScope::Social);

/** Short-term consumer + volunteer provider default (N020 / N023). Excludes Public. */
inline constexpr RelayScopeMask kRelayScopeShortTerm = kRelayScopeLinkSiteSocial |
                                                       static_cast<RelayScopeMask>(RelayScope::Org);

inline constexpr RelayScopeMask kRelayScopeVolunteerServe = kRelayScopeLinkSiteSocial;

inline bool RelayScopeMaskHas(RelayScopeMask mask, RelayScope scope) {
  return (mask & static_cast<RelayScopeMask>(scope)) != 0;
}

/** Whether an inbound relay dialer may use this node (N023 / nf). Header-only for libp2p glue. */
inline bool RelayAdmissionAllowsDialer(RelayScopeMask serve_mask, const std::string& dialer_peer_id,
                                       const std::unordered_set<std::string>& contact_peer_ids) {
  if (dialer_peer_id.empty()) {
    return false;
  }
  if (contact_peer_ids.empty()) {
    return true;
  }
  if (contact_peer_ids.find(dialer_peer_id) != contact_peer_ids.end()) {
    return true;
  }
  return RelayScopeMaskHas(serve_mask, RelayScope::Public);
}

} // namespace pbr
