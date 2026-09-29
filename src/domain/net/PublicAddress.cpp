#include "domain/net/PublicAddress.h"

#include <cstddef>

namespace pbr {
namespace {

bool AllZero(const uint8_t* bytes, const size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (bytes[i] != 0) {
      return false;
    }
  }
  return true;
}

/** Four network-order bytes → host-order IPv4. */
uint32_t EmbeddedIPv4(const uint8_t* addr) {
  return (static_cast<uint32_t>(addr[0]) << 24) | (static_cast<uint32_t>(addr[1]) << 16) |
         (static_cast<uint32_t>(addr[2]) << 8) | static_cast<uint32_t>(addr[3]);
}

} // namespace

bool IsPublicIPv4(const uint32_t host_order_addr) {
  const uint32_t a = host_order_addr;
  if ((a & 0xFF000000u) == 0x00000000u) return false;              // 0.0.0.0/8
  if ((a & 0xFF000000u) == 0x0A000000u) return false;              // 10.0.0.0/8
  if ((a & 0xFF000000u) == 0x7F000000u) return false;              // 127.0.0.0/8 loopback
  if ((a & 0xFFC00000u) == 0x64400000u) return false;              // 100.64.0.0/10 CGNAT
  if ((a & 0xFFFF0000u) == 0xA9FE0000u) return false;              // 169.254.0.0/16 link-local
  if ((a & 0xFFF00000u) == 0xAC100000u) return false;              // 172.16.0.0/12
  if ((a & 0xFFFF0000u) == 0xC0A80000u) return false;              // 192.168.0.0/16
  if ((a & 0xFFFFFF00u) == 0xC0000000u) return false;              // 192.0.0.0/24 IETF
  if ((a & 0xFFFFFF00u) == 0xC0000200u) return false;              // 192.0.2.0/24 TEST-NET-1
  if ((a & 0xFFFE0000u) == 0xC6120000u) return false;              // 198.18.0.0/15 benchmark
  if ((a & 0xFFFFFF00u) == 0xC6336400u) return false;              // 198.51.100.0/24 TEST-NET-2
  if ((a & 0xFFFFFF00u) == 0xCB007100u) return false;              // 203.0.113.0/24 TEST-NET-3
  if ((a & 0xF0000000u) == 0xE0000000u) return false;              // 224.0.0.0/4 multicast
  if ((a & 0xF0000000u) == 0xF0000000u) return false;              // 240.0.0.0/4 reserved + broadcast
  return true;
}

/*
 * ::, ::1, fe80::/10, fec0::/10 (deprecated site-local), fc00::/7 (ULA), ff00::/8 multicast, and
 * every IPv6 form that embeds an IPv4 address (::a.b.c.d, ::ffff:a.b.c.d, 64:ff9b::/96 NAT64,
 * 2002::/16 6to4) classified by the embedded address.
 */
bool IsPublicIPv6(const uint8_t* b) {
  if (AllZero(b, 15) && b[15] == 1) {
    return false; // ::1 loopback
  }
  if (AllZero(b, 16)) {
    return false; // :: unspecified
  }
  if (b[0] == 0x20 && b[1] == 0x02) {
    return IsPublicIPv4(EmbeddedIPv4(b + 2)); // 2002::/16 6to4
  }
  if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xFF && b[3] == 0x9B && AllZero(b + 4, 8)) {
    return IsPublicIPv4(EmbeddedIPv4(b + 12)); // 64:ff9b::/96 NAT64
  }
  if (AllZero(b, 10) && b[10] == 0xFF && b[11] == 0xFF) {
    return IsPublicIPv4(EmbeddedIPv4(b + 12)); // ::ffff:a.b.c.d IPv4-mapped
  }
  if (AllZero(b, 12)) {
    return IsPublicIPv4(EmbeddedIPv4(b + 12)); // ::a.b.c.d IPv4-compatible (deprecated)
  }
  if ((b[0] & 0xFE) == 0xFC) return false;               // fc00::/7 unique local
  if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return false; // fe80::/10 link-local
  if (b[0] == 0xFE && (b[1] & 0xC0) == 0xC0) return false; // fec0::/10 site-local (deprecated)
  if (b[0] == 0xFF) return false;                          // ff00::/8 multicast
  return true;
}

} // namespace pbr
