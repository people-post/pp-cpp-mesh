#include "mesh/dht/DhtKey.h"

#include "crypto/SodiumUtil.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>

namespace pp::mesh {
namespace {

constexpr std::string_view kBase58Alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr char kKeyDomain[] = "pp-mesh/dht/peer-routing/v1";  // followed by its NUL

} // namespace

std::optional<std::vector<uint8_t>> Base58Decode(const std::string_view text) {
  std::vector<uint8_t> bytes;  // little-endian accumulator
  for (const char c : text) {
    const size_t digit = kBase58Alphabet.find(c);
    if (digit == std::string_view::npos) {
      return std::nullopt;
    }
    uint32_t carry = static_cast<uint32_t>(digit);
    for (uint8_t& byte : bytes) {
      carry += static_cast<uint32_t>(byte) * 58;
      byte = static_cast<uint8_t>(carry & 0xff);
      carry >>= 8;
    }
    while (carry > 0) {
      bytes.push_back(static_cast<uint8_t>(carry & 0xff));
      carry >>= 8;
    }
  }
  for (const char c : text) {  // each leading '1' is a leading zero byte
    if (c != '1') {
      break;
    }
    bytes.push_back(0);
  }
  std::reverse(bytes.begin(), bytes.end());
  return bytes;
}

std::optional<DhtKey> DhtKeyOfPeerId(const std::string_view peer_id) {
  if (peer_id.empty()) {
    return std::nullopt;
  }
  auto multihash = Base58Decode(peer_id);
  if (!multihash || multihash->empty()) {
    return std::nullopt;
  }
  EnsureSodiumInit();
  std::vector<uint8_t> input(kKeyDomain, kKeyDomain + sizeof(kKeyDomain));  // includes the NUL
  input.insert(input.end(), multihash->begin(), multihash->end());
  DhtKey key{};
  crypto_generichash(key.data(), key.size(), input.data(), input.size(), nullptr, 0);
  return key;
}

DhtKey DhtDistance(const DhtKey& a, const DhtKey& b) {
  DhtKey out{};
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return out;
}

int DhtBucketIndex(const DhtKey& self, const DhtKey& b) {
  const DhtKey d = DhtDistance(self, b);
  for (size_t i = 0; i < d.size(); ++i) {
    if (d[i] != 0) {
      int bit = 7;
      while (((d[i] >> bit) & 1) == 0) {
        --bit;
      }
      return static_cast<int>((d.size() - 1 - i) * 8) + bit;
    }
  }
  return -1;
}

bool DhtCloser(const DhtKey& target, const DhtKey& a, const DhtKey& b) {
  for (size_t i = 0; i < target.size(); ++i) {
    const uint8_t da = static_cast<uint8_t>(a[i] ^ target[i]);
    const uint8_t db = static_cast<uint8_t>(b[i] ^ target[i]);
    if (da != db) {
      return da < db;
    }
  }
  return false;
}

} // namespace pp::mesh
