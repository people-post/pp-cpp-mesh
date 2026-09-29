#pragma once

#include <cstdint>

namespace pbr {

/**
 * Public-routable host checks for the post-DNS SSRF guard on peer-supplied URLs (`HttpClient`
 * with `restrict_to_public_https`). Pure byte logic — the socket plumbing lives in the
 * `PublicOnlySocket_*` backends.
 */

/** False for RFC 1918 / loopback / link-local / CGNAT / multicast / reserved / test ranges. */
bool IsPublicIPv4(uint32_t host_order_addr);

/**
 * `addr` is the 16 network-order bytes. False for ::, ::1, fe80::/10, fec0::/10, fc00::/7,
 * ff00::/8; IPv6 forms that embed an IPv4 address (::a.b.c.d, ::ffff:a.b.c.d, 64:ff9b::/96,
 * 2002::/16) are classified by the embedded address.
 */
bool IsPublicIPv6(const uint8_t* addr);

} // namespace pbr
