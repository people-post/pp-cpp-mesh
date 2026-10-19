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
  /** Seed-observed reflexive Amp multiaddr for this client (B26); may be set when dial_back_ok is false. */
  std::string dial_back_observed;
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
 * Build Amp dial-back probe targets (D8): global IPv6, public IPv4 + UPnP external + bound listen.
 * Preference: global `/ip6` before public `/ip4` before private listen.
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

/**
 * Global (non-link-local, non-ULA) `/ip6/.../adp/...` ADP multiaddrs for the Amp UDP port.
 * Uses `hosts` when provided; otherwise enumerates via `GlobalIpv6Addresses()`.
 */
std::vector<std::string> BuildAmpGlobalIpv6AdvertisedAddrs(
    const std::string& amp_listen_multiaddr, const std::string& local_peer_id,
    const std::vector<std::string>& hosts = {});

/** Dialable LAN IPv4 host strings (same filters as call-scoped advertise). */
std::vector<std::string> EnumerateDialableLanIpv4Hosts();

/**
 * Stable dial/advertise preference (H002/N013): global `/ip6` > public `/ip4` > private `/ip4` >
 * other. First entry is PreferredMultiaddr ingest order.
 */
std::vector<std::string> RankAmpDialMultiaddrs(std::vector<std::string> multiaddrs);

/** What the local host can actually dial; feeds RankAmpDialMultiaddrs(multiaddrs, ctx). */
struct AmpDialLocalContext {
  std::vector<std::string> lan_ipv4_hosts;  // dialable local LAN IPv4 hosts (same filters as advertise)
  bool has_global_ipv6 = false;             // false → Amp socket is v4-only; /ip6 peers are unsendable
};

/** Snapshot of the local interfaces for AmpDialLocalContext. */
AmpDialLocalContext CollectAmpDialLocalContext();

/**
 * RankAmpDialMultiaddrs with local knowledge: a private IPv4 on one of our /24s ranks first
 * (same-LAN call), and /ip6 is demoted below IPv4 when we have no global IPv6 to send from.
 * Ties fall back to AmpDialMultiaddrRank.
 */
std::vector<std::string> RankAmpDialMultiaddrs(std::vector<std::string> multiaddrs,
                                               const AmpDialLocalContext& ctx);

/** Rank key for tests (lower is preferred). */
int AmpDialMultiaddrRank(const std::string& multiaddr);

} // namespace pbr
