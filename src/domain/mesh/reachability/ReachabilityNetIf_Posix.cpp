#if !defined(_WIN32)

#include "domain/mesh/reachability/ReachabilityNetIf.h"

#include <algorithm>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace pbr::reachability_netif {
namespace {

bool IsIpv6LinkLocal(const in6_addr& addr) {
  return (addr.s6_addr[0] == 0xfe) && ((addr.s6_addr[1] & 0xc0) == 0x80);
}

bool IsIpv6UniqueLocal(const in6_addr& addr) {
  return (addr.s6_addr[0] == 0xfc) || (addr.s6_addr[0] == 0xfd);
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

std::vector<std::string> GlobalIpv6Addresses() {
  std::vector<std::string> out;
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
    AppendUnique(out, buf);
  }
  freeifaddrs(ifap);
  return out;
}

std::vector<std::string> PublicIpv4Addresses() {
  std::vector<std::string> out;
  ifaddrs* ifap = nullptr;
  if (getifaddrs(&ifap) != 0) {
    return out;
  }
  for (const ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    const auto* addr = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
    char buf[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
      continue;
    }
    AppendUnique(out, buf);
  }
  freeifaddrs(ifap);
  return out;
}

std::vector<LanIpv4If> LanIpv4Interfaces() {
  std::vector<LanIpv4If> out;
  ifaddrs* ifap = nullptr;
  if (getifaddrs(&ifap) != 0) {
    return out;
  }
  for (const ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_RUNNING) == 0 ||
        (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
      continue;
    }
    const auto* addr = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
    char buf[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
      continue;
    }
    LanIpv4If iface;
    iface.ip = buf;
    if (ifa->ifa_name) {
      iface.ifname = ifa->ifa_name;
    }
    iface.up_running_non_loopback = true;
    out.push_back(std::move(iface));
  }
  freeifaddrs(ifap);
  return out;
}

} // namespace pbr::reachability_netif

#endif // !defined(_WIN32)
