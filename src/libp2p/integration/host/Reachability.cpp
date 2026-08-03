#include "libp2p/integration/host/Reachability.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <sstream>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

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

std::string IpPrefixBeforeTcpLocal(const std::string& multiaddr) {
  const auto tcp_pos = multiaddr.find("/tcp/");
  if (tcp_pos == std::string::npos) {
    return multiaddr;
  }
  return multiaddr.substr(0, tcp_pos);
}

void AppendUnique(std::vector<std::string>& out, const std::string& value) {
  if (value.empty()) {
    return;
  }
  if (std::find(out.begin(), out.end(), value) == out.end()) {
    out.push_back(value);
  }
}

#if !defined(_WIN32)
bool IsIpv6LinkLocal(const in6_addr& addr) {
  return (addr.s6_addr[0] == 0xfe) && ((addr.s6_addr[1] & 0xc0) == 0x80);
}

bool IsIpv6UniqueLocal(const in6_addr& addr) {
  return (addr.s6_addr[0] == 0xfc) || (addr.s6_addr[0] == 0xfd);
}
#endif

std::optional<int> TcpPortFromMultiaddrLocal(const std::string& multiaddr) {
  const std::string marker = "/tcp/";
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

} // namespace

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

std::vector<std::string> EnumerateGlobalIpv6Addresses() {
  std::vector<std::string> out;
#if !defined(_WIN32)
  ifaddrs* ifap = nullptr;
  if (getifaddrs(&ifap) != 0) {
    return out;
  }
  for (const ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET6) {
      continue;
    }
    const auto* addr = reinterpret_cast<const sockaddr_in6*>(ifa->ifa_addr);
    if (IN6_IS_ADDR_LOOPBACK(&addr->sin6_addr) || IsIpv6LinkLocal(addr->sin6_addr) ||
        IsIpv6UniqueLocal(addr->sin6_addr)) {
      continue;
    }
    char buf[INET6_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET6, &addr->sin6_addr, buf, sizeof(buf))) {
      continue;
    }
    const std::string ip(buf);
    if (IsGlobalIpv6(ip)) {
      AppendUnique(out, ip);
    }
  }
  freeifaddrs(ifap);
#endif
  return out;
}

void AppendIpv6ListenCandidates(std::vector<std::string>& candidates, int tcp_port) {
  if (tcp_port <= 0 || tcp_port > 65535) {
    return;
  }
  for (const std::string& ip : EnumerateGlobalIpv6Addresses()) {
    AppendUnique(candidates, "/ip6/" + ip + "/tcp/" + std::to_string(tcp_port));
  }
}

bool ShouldSkipUpnpForListen(const std::string& bound_listen_multiaddr) {
  const auto tcp_pos = bound_listen_multiaddr.find("/tcp/");
  const std::string prefix =
      tcp_pos == std::string::npos ? bound_listen_multiaddr : bound_listen_multiaddr.substr(0, tcp_pos);
  const std::string ip = IpHostFromMultiaddrPrefix(prefix);
  if (ip.empty() || ip == "0.0.0.0") {
    return false;
  }
  return IsPublicIpv4(ip);
}

std::vector<std::string> BuildReachabilityProbeTargets(const std::string& bound_listen_multiaddr,
                                                       const std::string& local_peer_id,
                                                       const std::vector<std::string>& global_ipv6_addrs,
                                                       const std::string& upnp_external_ip) {
  std::vector<std::string> targets;
  const auto port = TcpPortFromMultiaddrLocal(bound_listen_multiaddr);
  if (!port || *port <= 0) {
    AppendUnique(targets, EnsurePeerIdSuffix(bound_listen_multiaddr, local_peer_id));
    return targets;
  }

  if (!upnp_external_ip.empty() && IsPublicIpv4(upnp_external_ip)) {
    AppendUnique(targets, EnsurePeerIdSuffix("/ip4/" + upnp_external_ip + "/tcp/" + std::to_string(*port),
                                             local_peer_id));
  }

  for (const std::string& ip : global_ipv6_addrs) {
    AppendUnique(targets, EnsurePeerIdSuffix("/ip6/" + ip + "/tcp/" + std::to_string(*port), local_peer_id));
  }

#if !defined(_WIN32)
  ifaddrs* ifap = nullptr;
  if (getifaddrs(&ifap) == 0) {
    for (const ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
        continue;
      }
      const auto* addr = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
      char buf[INET_ADDRSTRLEN] = {};
      if (!inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
        continue;
      }
      const std::string ip(buf);
      if (IsPublicIpv4(ip)) {
        AppendUnique(targets, EnsurePeerIdSuffix("/ip4/" + ip + "/tcp/" + std::to_string(*port), local_peer_id));
      }
    }
    freeifaddrs(ifap);
  }
#endif

  AppendUnique(targets, EnsurePeerIdSuffix(bound_listen_multiaddr, local_peer_id));
  return targets;
}

std::vector<std::string> BuildAdvertisedListenSet(const ReachabilitySignals& signals,
                                                  const std::string& bound_listen_multiaddr,
                                                  const std::string& local_peer_id,
                                                  const std::vector<std::string>& global_ipv6_addrs) {
  std::vector<std::string> out = BuildReachabilityProbeTargets(
      bound_listen_multiaddr, local_peer_id, global_ipv6_addrs, signals.upnp_external_ip);

  out.erase(std::remove_if(out.begin(), out.end(),
                           [](const std::string& ma) {
                             return ma.find("/ip4/0.0.0.0/") != std::string::npos ||
                                    ma.find("/ip6/::/") != std::string::npos;
                           }),
            out.end());

  if (signals.dial_back_ok && !signals.dial_back_dialed.empty()) {
    const auto it = std::find(out.begin(), out.end(), signals.dial_back_dialed);
    if (it != out.end() && it != out.begin()) {
      out.erase(it);
      out.insert(out.begin(), signals.dial_back_dialed);
    } else if (it == out.end()) {
      out.insert(out.begin(), signals.dial_back_dialed);
    }
  }

  if (out.empty() && !signals.listen_is_wildcard && !bound_listen_multiaddr.empty()) {
    AppendUnique(out, EnsurePeerIdSuffix(bound_listen_multiaddr, local_peer_id));
  }
  return out;
}

void AppendIpv6ListenCandidatesForPreferred(const std::string& preferred_multiaddr,
                                            std::vector<std::string>& candidates) {
  const auto port = TcpPortFromMultiaddrLocal(preferred_multiaddr);
  if (!port || *port <= 0) {
    return;
  }
  AppendIpv6ListenCandidates(candidates, *port);
}

std::vector<std::string> BuildMobileCallScopedAdvertisedAddrs(const std::string& bound_listen_multiaddr,
                                                              const std::string& local_peer_id) {
  std::vector<std::string> out;
  if (local_peer_id.empty()) {
    return out;
  }
  const auto port = TcpPortFromMultiaddrLocal(bound_listen_multiaddr);
  if (!port || *port <= 0) {
    return out;
  }

#if defined(_WIN32)
  ULONG size = 0;
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                        GAA_FLAG_SKIP_DNS_SERVER,
                           nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW ||
      size == 0) {
    return out;
  }
  std::vector<uint8_t> buffer(size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                        GAA_FLAG_SKIP_DNS_SERVER,
                           nullptr, adapters, &size) != NO_ERROR) {
    return out;
  }
  for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp) {
      continue;
    }
    if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      continue;
    }
    for (IP_ADAPTER_UNICAST_ADDRESS* uni = adapter->FirstUnicastAddress; uni != nullptr; uni = uni->Next) {
      if (!uni->Address.lpSockaddr || uni->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }
      const auto* addr = reinterpret_cast<const sockaddr_in*>(uni->Address.lpSockaddr);
      char buf[INET_ADDRSTRLEN] = {};
      if (!InetNtopA(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
        continue;
      }
      const std::string ip(buf);
      if (ip == "127.0.0.1" || !IsPrivateIpv4(ip)) {
        continue;
      }
      AppendUnique(out, EnsurePeerIdSuffix("/ip4/" + ip + "/tcp/" + std::to_string(*port), local_peer_id));
    }
  }
#else
  ifaddrs* ifap = nullptr;
  if (getifaddrs(&ifap) == 0) {
    for (const ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
        continue;
      }
      if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
        continue;
      }
      const auto* addr = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
      char buf[INET_ADDRSTRLEN] = {};
      if (!inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
        continue;
      }
      const std::string ip(buf);
      if (ip == "127.0.0.1" || !IsPrivateIpv4(ip)) {
        continue;
      }
      AppendUnique(out, EnsurePeerIdSuffix("/ip4/" + ip + "/tcp/" + std::to_string(*port), local_peer_id));
    }
    freeifaddrs(ifap);
  }
#endif

  return out;
}

} // namespace pbr
