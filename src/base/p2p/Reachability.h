#pragma once

#include "common/Error.h"

#include <chrono>
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
 * Build dial-back probe targets for reachability (nr / nu).
 * Uses interface IPs, optional UPnP external IP, global IPv6, and bound listen port.
 */
std::vector<std::string> BuildReachabilityProbeTargets(const std::string& bound_listen_multiaddr,
                                                       const std::string& local_peer_id,
                                                       const std::vector<std::string>& global_ipv6_addrs,
                                                       const std::string& upnp_external_ip);

/**
 * Build dialable multiaddrs to advertise via Identify for this Node (media-hop L2).
 * Merges bound listen, UPnP external, global IPv6, public interfaces, and dial-back-confirmed addr.
 */
std::vector<std::string> BuildAdvertisedListenSet(const ReachabilitySignals& signals,
                                                  const std::string& bound_listen_multiaddr,
                                                  const std::string& local_peer_id,
                                                  const std::vector<std::string>& global_ipv6_addrs);

/** Enumerate usable global IPv6 addresses on local interfaces. */
std::vector<std::string> EnumerateGlobalIpv6Addresses();

/** Append `/ip6/<global>/tcp/<port>` listen candidates (nu). */
void AppendIpv6ListenCandidates(std::vector<std::string>& candidates, int tcp_port);

/** True when listen IP is public — org pp-node seeds skip UPnP (N013). */
bool ShouldSkipUpnpForListen(const std::string& bound_listen_multiaddr);

/** Append global IPv6 listen candidates for the same TCP port (nu). */
void AppendIpv6ListenCandidatesForPreferred(const std::string& preferred_multiaddr,
                                            std::vector<std::string>& candidates);

/**
 * LAN private IPv4 listen addrs for mobile call-scoped publish (N025).
 * Skips reachability probe; enumerates carrier-up non-loopback private IPs.
 * Filters virtual NICs (virbr/docker/…) and libvirt default 192.168.122.0/24.
 */
std::vector<std::string> BuildMobileCallScopedAdvertisedAddrs(const std::string& bound_listen_multiaddr,
                                                              const std::string& local_peer_id);

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
