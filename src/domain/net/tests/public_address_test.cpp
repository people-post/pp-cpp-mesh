#include "domain/net/PublicAddress.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace pbr {
namespace {

uint32_t V4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (uint32_t{a} << 24) | (uint32_t{b} << 16) | (uint32_t{c} << 8) | uint32_t{d};
}

// Peer-supplied fetch URLs must only reach public Internet hosts (post-DNS SSRF guard).
TEST(PublicAddressTest, IPv4PrivateAndSpecialRangesAreNotPublic) {
  EXPECT_TRUE(IsPublicIPv4(V4(8, 8, 8, 8)));
  EXPECT_TRUE(IsPublicIPv4(V4(1, 1, 1, 1)));
  for (const uint32_t addr : {V4(10, 0, 0, 1), V4(127, 0, 0, 1), V4(192, 168, 1, 1), V4(172, 16, 0, 1),
                              V4(172, 31, 255, 255), V4(169, 254, 1, 1), V4(100, 64, 0, 1), V4(0, 0, 0, 0),
                              V4(224, 0, 0, 1), V4(255, 255, 255, 255), V4(198, 18, 117, 2)}) {
    EXPECT_FALSE(IsPublicIPv4(addr)) << addr;
  }
  EXPECT_TRUE(IsPublicIPv4(V4(172, 32, 0, 1))) << "just outside 172.16/12";
}

TEST(PublicAddressTest, IPv6LocalRangesAndEmbeddedIPv4) {
  std::array<uint8_t, 16> any{};
  EXPECT_FALSE(IsPublicIPv6(any.data())) << "::";
  auto loopback = any;
  loopback[15] = 1;
  EXPECT_FALSE(IsPublicIPv6(loopback.data())) << "::1";
  const std::array<uint8_t, 16> google{0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88};
  EXPECT_TRUE(IsPublicIPv6(google.data()));
  const std::array<uint8_t, 16> link_local{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  EXPECT_FALSE(IsPublicIPv6(link_local.data()));
  const std::array<uint8_t, 16> ula{0xfd, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  EXPECT_FALSE(IsPublicIPv6(ula.data()));
  const std::array<uint8_t, 16> mapped_private{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 10, 0, 0, 1};
  EXPECT_FALSE(IsPublicIPv6(mapped_private.data())) << "::ffff:10.0.0.1";
  const std::array<uint8_t, 16> mapped_public{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 8, 8, 8, 8};
  EXPECT_TRUE(IsPublicIPv6(mapped_public.data())) << "::ffff:8.8.8.8";
  const std::array<uint8_t, 16> sixto4_private{0x20, 0x02, 192, 168, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  EXPECT_FALSE(IsPublicIPv6(sixto4_private.data())) << "2002:c0a8:0001::1";
}

} // namespace
} // namespace pbr
