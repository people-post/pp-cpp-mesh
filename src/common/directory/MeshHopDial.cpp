#include "common/directory/MeshHopDial.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <sstream>

namespace pbr {
namespace {

std::string Ip4HostFromMultiaddr(const std::string& multiaddr) {
  const std::string marker = "/ip4/";
  const auto pos = multiaddr.find(marker);
  if (pos == std::string::npos) {
    return {};
  }
  const auto start = pos + marker.size();
  const auto end = multiaddr.find('/', start);
  if (end == std::string::npos) {
    return multiaddr.substr(start);
  }
  return multiaddr.substr(start, end - start);
}

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

bool IsPrivateIpv4Host(const std::string& ip) {
  std::array<int, 4> octets{};
  if (!ParseIpv4Octets(ip, octets)) {
    return false;
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
  return false;
}

int DialMultiaddrPreferenceRank(const std::string& multiaddr) {
  if (multiaddr.empty()) {
    return 100;
  }
  if (multiaddr.rfind("/ip6/", 0) == 0 && MultiaddrHasPublicDialHost(multiaddr)) {
    return 0;
  }
  if (multiaddr.find("/ip4/") != std::string::npos && MultiaddrHasPublicDialHost(multiaddr)) {
    return 10;
  }
  if (multiaddr.find("/adp/") != std::string::npos) {
    return MultiaddrHasPrivateIpv4Host(multiaddr) ? 20 : 15;
  }
  if (multiaddr.find("/ip4/") != std::string::npos || multiaddr.rfind("/ip6/", 0) == 0) {
    return 30;
  }
  return 50;
}

} // namespace

bool IsSameIpv4Subnet24(const std::string& multiaddr_a, const std::string& multiaddr_b) {
  const std::string ip_a = Ip4HostFromMultiaddr(multiaddr_a);
  const std::string ip_b = Ip4HostFromMultiaddr(multiaddr_b);
  if (ip_a.empty() || ip_b.empty()) {
    return false;
  }
  std::array<int, 4> a{};
  std::array<int, 4> b{};
  if (!ParseIpv4Octets(ip_a, a) || !ParseIpv4Octets(ip_b, b)) {
    return false;
  }
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

bool MultiaddrHasPrivateIpv4Host(const std::string& multiaddr) {
  const std::string ip = Ip4HostFromMultiaddr(multiaddr);
  return !ip.empty() && IsPrivateIpv4Host(ip);
}

bool MultiaddrHasPublicDialHost(const std::string& multiaddr) {
  auto ip6_host = [](const std::string& ma) -> std::string {
    if (ma.rfind("/ip6/", 0) != 0) {
      return {};
    }
    const size_t start = 5;
    const size_t end = ma.find('/', start);
    std::string host = ma.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
      host = host.substr(1, host.size() - 2);
    }
    return host;
  };
  auto is_global_ipv6 = [](const std::string& addr) {
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
    return addr != "::1" && addr != "::";
  };
  auto is_public_ipv4 = [](const std::string& ip) {
    if (ip.empty() || ip == "0.0.0.0" || ip == "127.0.0.1") {
      return false;
    }
    return !IsPrivateIpv4Host(ip);
  };

  if (multiaddr.rfind("/ip6/", 0) == 0) {
    return is_global_ipv6(ip6_host(multiaddr));
  }
  if (multiaddr.find("/ip4/") != std::string::npos) {
    const std::string ip = Ip4HostFromMultiaddr(multiaddr);
    return is_public_ipv4(ip);
  }
  return false;
}

bool CircuitHopDialBookAllowsRegister(const std::string& multiaddr) {
  return MultiaddrHasPublicDialHost(multiaddr);
}

bool CircuitHopMultiaddrIsUdpDialable(const std::string& multiaddr) {
  if (multiaddr.empty()) {
    return false;
  }
  if (multiaddr.rfind("/ip6/", 0) == 0) {
    const size_t start = 5;
    const size_t end = multiaddr.find('/', start);
    std::string host =
        multiaddr.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
      host = host.substr(1, host.size() - 2);
    }
    return !host.empty() && host != "::";
  }
  if (multiaddr.find("/ip4/") != std::string::npos) {
    const std::string ip = Ip4HostFromMultiaddr(multiaddr);
    return !ip.empty() && ip != "0.0.0.0";
  }
  // Non-IP (MemoryIo harness / future transports).
  return true;
}

std::string PreferredDialMultiaddr(const std::vector<std::string>& multiaddrs) {
  std::string best;
  int best_rank = 100;
  for (const std::string& ma : multiaddrs) {
    if (ma.empty()) {
      continue;
    }
    const int rank = DialMultiaddrPreferenceRank(ma);
    if (rank < best_rank) {
      best_rank = rank;
      best = ma;
    }
  }
  return best;
}

std::vector<std::string> OrderDialMultiaddrsWorstToBest(std::vector<std::string> multiaddrs) {
  std::stable_sort(multiaddrs.begin(), multiaddrs.end(),
                   [](const std::string& a, const std::string& b) {
                     const int ra = DialMultiaddrPreferenceRank(a);
                     const int rb = DialMultiaddrPreferenceRank(b);
                     // Higher rank = worse; register worst first for last-write-wins.
                     if (ra != rb) {
                       return ra > rb;
                     }
                     return a < b;
                   });
  return multiaddrs;
}

std::vector<MeshHopCandidate> PreferLocalMediaHop(std::vector<MeshHopCandidate> ranked,
                                                  const std::string& local_peer_id,
                                                  const std::string& local_multiaddr) {
  if (local_peer_id.empty()) {
    return ranked;
  }
  std::vector<MeshHopCandidate> out;
  out.reserve(ranked.size() + 1);
  MeshHopCandidate local;
  local.peer_id = local_peer_id;
  local.multiaddr = local_multiaddr;
  local.affinity = MeshHopAffinity::Contact;
  local.dialable = true;
  out.push_back(std::move(local));
  for (MeshHopCandidate& c : ranked) {
    if (c.peer_id == local_peer_id) {
      continue;
    }
    out.push_back(std::move(c));
  }
  return out;
}

} // namespace pbr
