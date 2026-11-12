#pragma once

#include "common/Error.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
namespace pp::mesh {

namespace lan_mdns {
class UdpMulticastSocket;
}

/** mDNS service type for LAN peer discovery (ns2). `_udp`: the service is Amp over ADP (UDP). */
inline constexpr const char* kLanMdnsServiceType = "_pp-mesh._udp.local";

struct LanMdnsDiscoveredPeer {
  std::string peer_id_base58;
  std::string host_ip;
  /** Amp UDP listen port (the SRV record's port). */
  int amp_udp_port = 0;
};

/**
 * The discovery thread's reads that bring nothing: a socket that keeps saying "readable" while every read comes
 * back empty or failed (an interface gone, a dead multicast membership) spun a whole core in a wait-read loop
 * (pp-browser #596). Each such read waits a little longer (50 ms, doubling to 1 s), and every eighth reopens the
 * socket; a packet ends the run. Pure: the thread waits and reopens as it says.
 */
class LanMdnsEmptyReads {
public:
  struct Next {
    std::chrono::milliseconds wait{0};
    bool reopen = false;
  };
  static constexpr std::chrono::milliseconds kFirstWait{50};
  static constexpr std::chrono::milliseconds kMaxWait{1000};
  static constexpr int kReopenEvery = 8;

  Next OnEmpty() {
    ++run_;
    wait_ = wait_.count() == 0 ? kFirstWait : std::min(wait_ * 2, kMaxWait);
    return Next{wait_, run_ % kReopenEvery == 0};
  }
  void OnPacket() {
    run_ = 0;
    wait_ = std::chrono::milliseconds(0);
  }

private:
  int run_ = 0;
  std::chrono::milliseconds wait_{0};
};

/**
 * Minimal multicast DNS announcer/browser for contacts-only LAN discovery (ns2).
 * Announces the local PeerId (TXT `peer=`), its amp wire version (TXT `wire=`) and the Amp UDP listen port (SRV)
 * when listening; an announcement of another wire version, or one that says none, is not a browse result. Browse
 * results are filtered upstream.
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

  /** Update what we announce (PeerId + Amp UDP listen port). Empty peer_id stops announce. */
  void SetAdvertisement(const std::string& peer_id_base58, int amp_udp_port,
                        const std::vector<std::string>& lan_ipv4_addrs);

  /** Build `/ip4/…/udp/…/adp/<wire>/p2p/…` from a browse result (always of our wire version). */
  static std::optional<std::string> BuildAdpMultiaddr(const LanMdnsDiscoveredPeer& peer);

  /** The announcement packet (PTR, SRV with the Amp UDP port, TXT `peer=` `wire=`, A per address); empty when incomplete. */
  static std::vector<uint8_t> EncodeAnnouncement(const std::string& peer_id_base58, int amp_udp_port,
                                                 const std::vector<std::string>& lan_ipv4_addrs);
  /** The peers an mDNS packet announces (each with an address and a port). */
  static std::vector<LanMdnsDiscoveredPeer> ParseAnnouncement(const uint8_t* data, size_t len);

  /** Test helpers — DNS name encode/decode (no socket I/O). */
  static Roe<std::vector<uint8_t>> EncodeDnsName(const std::string& fqdn);
  static Roe<std::string> DecodeDnsName(const std::vector<uint8_t>& packet, size_t offset, size_t* out_next);

private:
  void ThreadMain();
  void SendBrowseQuery(lan_mdns::UdpMulticastSocket& socket);
  void SendAnnouncement(lan_mdns::UdpMulticastSocket& socket);
  void HandlePacket(const uint8_t* data, size_t len);

  DiscoveredFn on_discovered_;
  mutable std::mutex advertise_mutex_;
  std::string advertise_peer_id_;
  int advertise_amp_udp_port_ = 0;
  std::vector<std::string> advertise_ips_;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread thread_;
};

} // namespace pp::mesh
