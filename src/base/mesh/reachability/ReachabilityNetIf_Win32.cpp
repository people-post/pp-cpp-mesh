#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "base/mesh/reachability/ReachabilityNetIf.h"

#include <cstdint>
#include <vector>

namespace pbr::reachability_netif {

std::vector<std::string> GlobalIpv6Addresses() {
  return {};
}

std::vector<std::string> PublicIpv4Addresses() {
  return {};
}

std::vector<LanIpv4If> LanIpv4Interfaces() {
  std::vector<LanIpv4If> out;
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
    for (IP_ADAPTER_UNICAST_ADDRESS* uni = adapter->FirstUnicastAddress; uni != nullptr;
         uni = uni->Next) {
      if (!uni->Address.lpSockaddr || uni->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }
      const auto* addr = reinterpret_cast<const sockaddr_in*>(uni->Address.lpSockaddr);
      char buf[INET_ADDRSTRLEN] = {};
      if (!InetNtopA(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
        continue;
      }
      LanIpv4If iface;
      iface.ip = buf;
      if (adapter->AdapterName != nullptr) {
        iface.ifname = adapter->AdapterName;
      }
      iface.up_running_non_loopback = true;
      out.push_back(std::move(iface));
    }
  }
  return out;
}

} // namespace pbr::reachability_netif

#endif // defined(_WIN32)
