#pragma once

#include "common/Error.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace pbr {

/** mDNS service type for pp-browser LAN peer discovery (ns2). */
inline constexpr const char* kLanMdnsServiceType = "_pp-browser._tcp.local";

struct LanMdnsDiscoveredPeer {
  std::string peer_id_base58;
  std::string host_ip;
  int tcp_port = 0;
};

/**
 * Minimal multicast DNS announcer/browser for contacts-only LAN discovery (ns2).
 * Announces local PeerId + libp2p TCP port when listening; browse results are filtered upstream.
 */
class LanMdnsDiscovery {
public:
  using DiscoveredFn = std::function<void(LanMdnsDiscoveredPeer)>;

  LanMdnsDiscovery();
  ~LanMdnsDiscovery();

  LanMdnsDiscovery(const LanMdnsDiscovery&) = delete;
  LanMdnsDiscovery& operator=(const LanMdnsDiscovery&) = delete;

  void SetOnDiscovered(DiscoveredFn callback);

  /** Start announce + browse loops. Idempotent. */
  Roe<void> Start();
  void Stop();

  bool IsRunning() const { return running_.load(); }

  /** Update what we announce (PeerId + bound TCP listen port). Empty peer_id stops announce. */
  void SetAdvertisement(const std::string& peer_id_base58, int tcp_port,
                        const std::vector<std::string>& lan_ipv4_addrs);

  /** Build `/ip4/…/tcp/…/p2p/…` from a browse result. */
  static std::optional<std::string> BuildMultiaddr(const LanMdnsDiscoveredPeer& peer);

  /** Test helpers — DNS name encode/decode (no socket I/O). */
  static Roe<std::vector<uint8_t>> EncodeDnsName(const std::string& fqdn);
  static Roe<std::string> DecodeDnsName(const std::vector<uint8_t>& packet, size_t offset, size_t* out_next);

private:
  void ThreadMain();
  void SendBrowseQuery(int socket_fd);
  void SendAnnouncement(int socket_fd);
  void HandlePacket(const uint8_t* data, size_t len);

  DiscoveredFn on_discovered_;
  mutable std::mutex advertise_mutex_;
  std::string advertise_peer_id_;
  int advertise_port_ = 0;
  std::vector<std::string> advertise_ips_;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread thread_;
};

} // namespace pbr
