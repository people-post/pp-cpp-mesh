#pragma once

#include "common/Error.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

enum class ReachabilityStatus {
  Unknown,
  Checking,
  Reachable,
  OutboundOnly,
  Blocked,
};

struct ReachabilitySignals {
  bool listen_is_wildcard = false;
  bool has_private_listen_ip = false;
  bool has_public_listen_ip = false;
  bool has_global_ipv6 = false;
  bool seed_dial_ok = false;
  bool dial_back_ok = false;
  bool upnp_mapped = false;
  std::string upnp_external_ip;
  int upnp_external_port = 0;
  std::string dial_back_dialed;
  std::string seed_dial_error;
  std::string dial_back_error;
};

struct ReachabilitySnapshot {
  ReachabilityStatus status = ReachabilityStatus::Unknown;
  ReachabilitySignals signals;
  std::chrono::steady_clock::time_point measured_at{};
};

/** Machine-readable status id for i18n / ops (`reachable`, `outbound_only`, …). */
const char* ReachabilityStatusKey(ReachabilityStatus status);

/** Guided help sheet id (`outbound_only`, `blocked`, `reachable`, empty). */
const char* ReachabilityHelpKey(ReachabilityStatus status);

ReachabilityStatus ClassifyReachability(const ReachabilitySignals& signals);

bool IsPrivateIpv4(const std::string& dotted_quad);
bool IsPublicIpv4(const std::string& dotted_quad);
bool IsGlobalIpv6(const std::string& addr);

/**
 * True for IPs that are private but typically undialable from LAN peers
 * (libvirt virbr default 192.168.122.0/24). Used to filter call/mDNS advertise.
 */
bool IsLikelyUndialableLanIpv4(const std::string& dotted_quad);

/** True for virtual NIC names (virbr*, docker*, veth*, …) that should not be advertised. */
bool IsVirtualLanIfaceName(const std::string& ifname);

/** Extract `/ip4/<addr>` or `/ip6/<addr>` host portion from a multiaddr prefix. */
std::string IpHostFromMultiaddrPrefix(const std::string& multiaddr);

/**
 * Build Amp dial-back probe targets (D8): public IPv4 + UPnP external + bound listen as ADP MAs.
 * (ADP multiaddrs are IPv4-only today.)
 */
std::vector<std::string> BuildAmpReachabilityProbeTargets(const std::string& amp_listen_multiaddr,
                                                          const std::string& local_peer_id,
                                                          const std::string& upnp_external_ip);

/** UDP port from `/udp/<n>` in a multiaddr, if present. */
std::optional<int> UdpPortFromMultiaddr(const std::string& multiaddr);

/** True when listen IP is public — org pp-node seeds skip UPnP (N013). */
bool ShouldSkipUpnpForListen(const std::string& bound_listen_multiaddr);

/**
 * LAN private IPv4 ADP multiaddrs for Amp PreferLocal / invite advertise (D10).
 * Expands a (possibly wildcard) Amp listen MA across dialable LAN interface IPs.
 * If no LAN IPs, returns the input multiaddr when it already has a concrete host.
 */
std::vector<std::string> BuildAmpLanAdvertisedAddrs(const std::string& amp_listen_multiaddr,
                                                    const std::string& local_peer_id);

/** Dialable LAN IPv4 host strings (same filters as call-scoped advertise). */
std::vector<std::string> EnumerateDialableLanIpv4Hosts();

} // namespace pbr
