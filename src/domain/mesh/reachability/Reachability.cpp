#include "domain/mesh/reachability/Reachability.h"
#include "domain/mesh/reachability/ReachabilityNetIf.h"
#include "amp/link/AdpMultiaddr.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <sstream>

namespace pbr {

namespace {

bool ParseIpv4Octets(const std::string& ip, std::array<int, 4>& out) {
  std::istringstream ss(ip);
  std::string part;
  for (int i = 0; i < 4; ++i) {
    if (!std::getline(ss, part, '.') || part.empty()) {
      return false;
    }
    char* end = nullptr;
    const long v = std::strtol(part.c_str(), &end, 10);
    if (end == part.c_str() || v < 0 || v > 255) {
      return false;
    }
    out[static_cast<size_t>(i)] = static_cast<int>(v);
  }
  return !std::getline(ss, part, '.');
}

std::string EnsurePeerIdSuffix(std::string multiaddr, const std::string& peer_id) {
  if (peer_id.empty() || multiaddr.find("/p2p/") != std::string::npos) {
    return multiaddr;
  }
  if (multiaddr.empty() || multiaddr.back() != '/') {
    multiaddr += "/p2p/" + peer_id;
  } else {
    multiaddr += "p2p/" + peer_id;
  }
  return multiaddr;
}

void AppendUnique(std::vector<std::string>& out, const std::string& value) {
  if (value.empty()) {
    return;
  }
  if (std::find(out.begin(), out.end(), value) == out.end()) {
    out.push_back(value);
  }
}

} // namespace

std::optional<int> UdpPortFromMultiaddr(const std::string& multiaddr) {
  const std::string marker = "/udp/";
  const auto pos = multiaddr.find(marker);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  size_t i = pos + marker.size();
  if (i >= multiaddr.size() || !std::isdigit(static_cast<unsigned char>(multiaddr[i]))) {
    return std::nullopt;
  }
  char* end = nullptr;
  const long port = std::strtol(multiaddr.c_str() + static_cast<std::ptrdiff_t>(i), &end, 10);
  if (end == multiaddr.c_str() + static_cast<std::ptrdiff_t>(i) || port < 0 || port > 65535) {
    return std::nullopt;
  }
  return static_cast<int>(port);
}

const char* ReachabilityStatusKey(ReachabilityStatus status) {
  switch (status) {
  case ReachabilityStatus::Checking:
    return "checking";
  case ReachabilityStatus::Reachable:
    return "reachable";
  case ReachabilityStatus::OutboundOnly:
    return "outbound_only";
  case ReachabilityStatus::Blocked:
    return "blocked";
  case ReachabilityStatus::Unknown:
  default:
    return "unknown";
  }
}

const char* ReachabilityHelpKey(ReachabilityStatus status) {
  switch (status) {
  case ReachabilityStatus::OutboundOnly:
    return "outbound_only";
  case ReachabilityStatus::Blocked:
    return "blocked";
  case ReachabilityStatus::Reachable:
    return "reachable";
  default:
    return "";
  }
}

ReachabilityStatus ClassifyReachability(const ReachabilitySignals& signals) {
  if (!signals.seed_dial_ok) {
    return ReachabilityStatus::Blocked;
  }
  if (signals.dial_back_ok) {
    return ReachabilityStatus::Reachable;
  }
  return ReachabilityStatus::OutboundOnly;
}

bool IsPrivateIpv4(const std::string& dotted_quad) {
  if (dotted_quad == "0.0.0.0" || dotted_quad == "127.0.0.1") {
    return true;
  }
  std::array<int, 4> octets{};
  if (!ParseIpv4Octets(dotted_quad, octets)) {
    return true;
  }
  if (octets[0] == 10) {
    return true;
  }
  if (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) {
    return true;
  }
  if (octets[0] == 192 && octets[1] == 168) {
    return true;
  }
  if (octets[0] == 169 && octets[1] == 254) {
    return true;
  }
  return false;
}

bool IsPublicIpv4(const std::string& dotted_quad) {
  if (dotted_quad.empty()) {
    return false;
  }
  return !IsPrivateIpv4(dotted_quad);
}

bool IsLikelyUndialableLanIpv4(const std::string& dotted_quad) {
  // libvirt virbr0 default NAT network — UP/NO-CARRIER on many Linux desktops; phones cannot
  // route to it. Dogfood: Samsung mDNS kept overwriting CallSfuAttach /ip4/192.168.1.x with this.
  std::array<int, 4> octets{};
  if (!ParseIpv4Octets(dotted_quad, octets)) {
    return false;
  }
  return octets[0] == 192 && octets[1] == 168 && octets[2] == 122;
}

bool IsVirtualLanIfaceName(const std::string& ifname) {
  if (ifname.empty()) {
    return false;
  }
  auto starts_with = [&](const char* prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    return ifname.size() >= n && ifname.compare(0, n, prefix) == 0;
  };
  return starts_with("virbr") || starts_with("docker") || starts_with("veth") ||
         starts_with("lxcbr") || starts_with("br-") || starts_with("vmnet") ||
         starts_with("vboxnet") || starts_with("zt") || starts_with("tailscale") ||
         ifname == "docker0";
}

bool IsGlobalIpv6(const std::string& addr) {
  if (addr.empty() || addr.find(':') == std::string::npos) {
    return false;
  }
  if (addr.rfind("fe80:", 0) == 0 || addr.rfind("FE80:", 0) == 0) {
    return false;
  }
  if (addr.rfind("fc", 0) == 0 || addr.rfind("fd", 0) == 0 || addr.rfind("FC", 0) == 0 ||
      addr.rfind("FD", 0) == 0) {
    return false;
  }
  if (addr == "::1") {
    return false;
  }
  return true;
}

std::string IpHostFromMultiaddrPrefix(const std::string& multiaddr) {
  if (multiaddr.rfind("/ip4/", 0) == 0) {
    const size_t start = 5;
    const size_t end = multiaddr.find('/', start);
    return multiaddr.substr(start, end == std::string::npos ? std::string::npos : end - start);
  }
  if (multiaddr.rfind("/ip6/", 0) == 0) {
    const size_t start = 5;
    const size_t end = multiaddr.find('/', start);
    std::string host = multiaddr.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
      host = host.substr(1, host.size() - 2);
    }
    return host;
  }
  return {};
}

bool ShouldSkipUpnpForListen(const std::string& bound_listen_multiaddr) {
  const auto tcp_pos = bound_listen_multiaddr.find("/tcp/");
  const auto udp_pos = bound_listen_multiaddr.find("/udp/");
  size_t cut = std::string::npos;
  if (tcp_pos != std::string::npos) {
    cut = tcp_pos;
  }
  if (udp_pos != std::string::npos && (cut == std::string::npos || udp_pos < cut)) {
    cut = udp_pos;
  }
  const std::string prefix =
      cut == std::string::npos ? bound_listen_multiaddr : bound_listen_multiaddr.substr(0, cut);
  const std::string ip = IpHostFromMultiaddrPrefix(prefix);
  if (ip.empty() || ip == "0.0.0.0") {
    return false;
  }
  return IsPublicIpv4(ip);
}

std::vector<std::string> BuildAmpReachabilityProbeTargets(const std::string& amp_listen_multiaddr,
                                                          const std::string& local_peer_id,
                                                          const std::string& upnp_external_ip) {
  std::vector<std::string> targets;
  const auto port = UdpPortFromMultiaddr(amp_listen_multiaddr);
  if (!port || *port <= 0) {
    AppendUnique(targets, EnsurePeerIdSuffix(amp_listen_multiaddr, local_peer_id));
    return targets;
  }

  auto append_adp = [&](const std::string& ip) {
    if (ip.empty() || ip == "0.0.0.0" || ip == "127.0.0.1") {
      return;
    }
    AppendUnique(targets, EnsurePeerIdSuffix("/ip4/" + ip + "/udp/" + std::to_string(*port) + "/adp/1.0.0",
                                             local_peer_id));
  };

  if (!upnp_external_ip.empty() && IsPublicIpv4(upnp_external_ip)) {
    append_adp(upnp_external_ip);
  }
  for (const std::string& ip : reachability_netif::PublicIpv4Addresses()) {
    if (IsPublicIpv4(ip)) {
      append_adp(ip);
    }
  }
  AppendUnique(targets, EnsurePeerIdSuffix(amp_listen_multiaddr, local_peer_id));
  return targets;
}

std::vector<std::string> EnumerateDialableLanIpv4Hosts() {
  std::vector<std::string> out;
  for (const auto& iface : reachability_netif::LanIpv4Interfaces()) {
    if (!iface.up_running_non_loopback) {
      continue;
    }
    if (!iface.ifname.empty() && IsVirtualLanIfaceName(iface.ifname)) {
      continue;
    }
    const std::string& ip = iface.ip;
    if (ip == "127.0.0.1" || !IsPrivateIpv4(ip) || IsLikelyUndialableLanIpv4(ip)) {
      continue;
    }
    AppendUnique(out, ip);
  }
  return out;
}

std::vector<std::string> BuildAmpLanAdvertisedAddrs(const std::string& amp_listen_multiaddr,
                                                   const std::string& local_peer_id) {
  std::vector<std::string> out;
  if (amp_listen_multiaddr.empty() || local_peer_id.empty()) {
    return out;
  }
  auto parsed = pp::amp::ParseAdpMultiaddr(amp_listen_multiaddr);
  if (!parsed) {
    return out;
  }
  const uint16_t port = parsed->endpoint.port;
  if (port == 0) {
    return out;
  }
  const std::string peer =
      !parsed->peer_id.empty() ? parsed->peer_id : local_peer_id;
  for (const std::string& ip : EnumerateDialableLanIpv4Hosts()) {
    std::array<int, 4> octets{};
    if (!ParseIpv4Octets(ip, octets)) {
      continue;
    }
    auto ep = pp::adp::IpEndpoint::V4(static_cast<uint8_t>(octets[0]), static_cast<uint8_t>(octets[1]),
                                  static_cast<uint8_t>(octets[2]), static_cast<uint8_t>(octets[3]), port);
    if (auto formatted = pp::amp::FormatAdpMultiaddr(ep, peer)) {
      AppendUnique(out, *formatted);
    }
  }
  if (out.empty()) {
    // No LAN expansion — keep concrete non-wildcard listen if already dialable.
    const std::string host = IpHostFromMultiaddrPrefix(amp_listen_multiaddr);
    if (!host.empty() && host != "0.0.0.0" && host != "::") {
      AppendUnique(out, EnsurePeerIdSuffix(amp_listen_multiaddr, peer));
    }
  }
  return out;
}

} // namespace pbr
