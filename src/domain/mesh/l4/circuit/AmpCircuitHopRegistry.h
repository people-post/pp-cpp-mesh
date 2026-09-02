#pragma once

#include "amp/L3/ChannelSession.h"
#include "domain/mesh/l4/circuit/CircuitBundleLogic.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace pbr {

/**
 * Amp analogue of PeerSessionManager circuit hop table ([A020] / D9 step 5c).
 * Maps (peer_key × target_protocol) → bridged ChannelSession from CircuitTunnelCoordinator.
 * OpenChannel is not used for adopted hops — L4 coordinators Bind/SetFrameHandler on the session.
 */
class AmpCircuitHopRegistry {
public:
  struct Hop {
    std::shared_ptr<pp::amp::ChannelSession> session;
    std::string relay_peer_key;
    std::string target_protocol;
    CircuitTunnelId tunnel_id{};
  };

  static std::string Key(const std::string& peer_key, const std::string& target_protocol);

  Roe<void> Install(const std::string& peer_key, const std::string& relay_peer_key,
                    const std::string& target_protocol, std::shared_ptr<pp::amp::ChannelSession> session,
                    CircuitTunnelId tunnel_id = {});

  std::optional<Hop> Find(const std::string& peer_key, const std::string& target_protocol) const;
  bool HasAny(const std::string& peer_key) const;

  void Clear(const std::string& peer_key);
  void Clear(const std::string& peer_key, const std::string& target_protocol);
  void ClearAll();

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, Hop> hops_;
};

} // namespace pbr
