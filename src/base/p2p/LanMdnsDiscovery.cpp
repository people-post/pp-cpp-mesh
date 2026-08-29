#include "base/p2p/LanMdnsDiscovery.h"
#include "base/p2p/LanMdnsSocket.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr uint16_t kDnsTypePtr = 12;
constexpr uint16_t kDnsTypeSrv = 33;
constexpr uint16_t kDnsTypeTxt = 16;
constexpr uint16_t kDnsTypeA = 1;
constexpr uint16_t kDnsClassIn = 1;
constexpr int kMdnsPort = 5353;
constexpr const char* kMdnsMulticast = "224.0.0.251";

void AppendU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(v & 0xff));
}

void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(v & 0xff));
}

bool ReadU16(const std::vector<uint8_t>& packet, size_t offset, uint16_t* out) {
  if (offset + 2 > packet.size()) {
    return false;
  }
  *out = static_cast<uint16_t>((static_cast<uint16_t>(packet[offset]) << 8) | packet[offset + 1]);
  return true;
}

/** Portable dotted-quad parser (avoids Winsock on Windows for packet helpers). */
bool ParseIpv4Dotted(const std::string& ip, uint8_t out[4]) {
  size_t start = 0;
  for (int i = 0; i < 4; ++i) {
    if (start >= ip.size()) {
      return false;
    }
    const size_t end = (i < 3) ? ip.find('.', start) : ip.size();
    if (i < 3 && end == std::string::npos) {
      return false;
    }
    if (end == start || end - start > 3) {
      return false;
    }
    int value = 0;
    for (size_t p = start; p < end; ++p) {
      if (ip[p] < '0' || ip[p] > '9') {
        return false;
      }
      value = value * 10 + (ip[p] - '0');
      if (value > 255) {
        return false;
      }
    }
    out[i] = static_cast<uint8_t>(value);
    start = end + 1;
  }
  return start == ip.size() + 1;
}

Roe<std::string> ReadDnsName(const std::vector<uint8_t>& packet, size_t offset, size_t* out_next) {
  std::string out;
  size_t pos = offset;
  bool jumped = false;
  size_t jump_return = 0;
  int guard = 0;
  while (pos < packet.size() && guard++ < 128) {
    const uint8_t len = packet[pos];
    if (len == 0) {
      pos++;
      if (!jumped) {
        *out_next = pos;
      } else {
        *out_next = jump_return;
      }
      return out;
    }
    if ((len & 0xC0) == 0xC0) {
      if (pos + 1 >= packet.size()) {
        return Error("dns name compression truncated");
      }
      const uint16_t ptr = static_cast<uint16_t>(((len & 0x3F) << 8) | packet[pos + 1]);
      if (!jumped) {
        jump_return = pos + 2;
        jumped = true;
      }
      pos = ptr;
      continue;
    }
    if (pos + 1 + len > packet.size()) {
      return Error("dns label truncated");
    }
    if (!out.empty()) {
      out.push_back('.');
    }
    out.append(reinterpret_cast<const char*>(packet.data() + pos + 1), len);
    pos += 1 + len;
  }
  return Error("dns name parse failed");
}

std::string SanitizeInstanceLabel(const std::string& peer_id) {
  std::string label;
  label.reserve(peer_id.size());
  for (char c : peer_id) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      label.push_back(c);
    } else {
      label.push_back('-');
    }
  }
  if (label.empty()) {
    label = "peer";
  }
  if (label.size() > 63) {
    label.resize(63);
  }
  return label;
}

std::string InstanceFqdn(const std::string& peer_id) {
  return SanitizeInstanceLabel(peer_id) + "." + kLanMdnsServiceType;
}

std::string HostTargetFqdn(const std::string& peer_id) {
  return SanitizeInstanceLabel(peer_id) + ".local";
}

std::vector<uint8_t> BuildBrowseQuery() {
  std::vector<uint8_t> packet(12);
  // Transaction id 0, standard query, one question.
  packet[2] = 0x00;
  packet[3] = 0x00;
  packet[4] = 0x00;
  packet[5] = 0x01; // QDCOUNT
  auto name = LanMdnsDiscovery::EncodeDnsName(kLanMdnsServiceType);
  if (!name) {
    return {};
  }
  packet.insert(packet.end(), name->begin(), name->end());
  AppendU16(packet, kDnsTypePtr);
  AppendU16(packet, kDnsClassIn);
  return packet;
}

void AppendRecord(std::vector<uint8_t>& packet, const std::string& name, uint16_t type, uint32_t ttl,
                  const std::vector<uint8_t>& rdata, bool additional) {
  auto encoded_name = LanMdnsDiscovery::EncodeDnsName(name);
  if (!encoded_name) {
    return;
  }
  packet.insert(packet.end(), encoded_name->begin(), encoded_name->end());
  AppendU16(packet, type);
  AppendU16(packet, kDnsClassIn);
  AppendU32(packet, ttl);
  AppendU16(packet, static_cast<uint16_t>(rdata.size()));
  packet.insert(packet.end(), rdata.begin(), rdata.end());
  (void)additional;
}

std::vector<uint8_t> BuildAnnouncement(const std::string& peer_id, int tcp_port,
                                       const std::vector<std::string>& lan_ips) {
  if (peer_id.empty() || tcp_port <= 0 || lan_ips.empty()) {
    return {};
  }
  const std::string instance = InstanceFqdn(peer_id);
  const std::string target = HostTargetFqdn(peer_id);

  std::vector<uint8_t> packet(12);
  packet[2] = 0x84; // response + authoritative
  packet[3] = 0x00;
  // Answer count filled later.
  const size_t answer_count_pos = 6;
  uint16_t answer_count = 0;

  // PTR: service → instance
  {
    std::vector<uint8_t> ptr_rdata;
    auto ptr_name = LanMdnsDiscovery::EncodeDnsName(instance);
    if (ptr_name) {
      ptr_rdata.insert(ptr_rdata.end(), ptr_name->begin(), ptr_name->end());
      AppendRecord(packet, kLanMdnsServiceType, kDnsTypePtr, 120, ptr_rdata, false);
      answer_count++;
    }
  }

  // SRV on instance
  {
    std::vector<uint8_t> srv;
    AppendU16(srv, 0); // priority
    AppendU16(srv, 0); // weight
    AppendU16(srv, static_cast<uint16_t>(tcp_port));
    auto target_name = LanMdnsDiscovery::EncodeDnsName(target);
    if (target_name) {
      srv.insert(srv.end(), target_name->begin(), target_name->end());
      AppendRecord(packet, instance, kDnsTypeSrv, 120, srv, true);
      answer_count++;
    }
  }

  // TXT peer id
  {
    const std::string txt_line = "peer=" + peer_id;
    std::vector<uint8_t> txt;
    txt.push_back(static_cast<uint8_t>(txt_line.size()));
    txt.insert(txt.end(), txt_line.begin(), txt_line.end());
    AppendRecord(packet, instance, kDnsTypeTxt, 120, txt, true);
    answer_count++;
  }

  for (const std::string& ip : lan_ips) {
    std::vector<uint8_t> rdata(4);
    if (!ParseIpv4Dotted(ip, rdata.data())) {
      continue;
    }
    AppendRecord(packet, target, kDnsTypeA, 120, rdata, true);
    answer_count++;
  }

  packet[answer_count_pos] = static_cast<uint8_t>((answer_count >> 8) & 0xff);
  packet[answer_count_pos + 1] = static_cast<uint8_t>(answer_count & 0xff);
  return packet;
}

std::optional<std::string> ParseTxtPeerId(const std::vector<uint8_t>& rdata) {
  size_t pos = 0;
  while (pos < rdata.size()) {
    const uint8_t len = rdata[pos++];
    if (pos + len > rdata.size()) {
      break;
    }
    const std::string line(reinterpret_cast<const char*>(rdata.data() + pos), len);
    pos += len;
    constexpr std::string_view kPrefix = "peer=";
    if (line.rfind(std::string(kPrefix), 0) == 0) {
      return line.substr(kPrefix.size());
    }
  }
  return std::nullopt;
}

std::optional<int> ParseSrvPort(const std::vector<uint8_t>& rdata) {
  if (rdata.size() < 6) {
    return std::nullopt;
  }
  return static_cast<int>((static_cast<uint16_t>(rdata[4]) << 8) | rdata[5]);
}

std::optional<std::string> ParseARecordIp(const std::vector<uint8_t>& rdata) {
  if (rdata.size() != 4) {
    return std::nullopt;
  }
  return std::to_string(rdata[0]) + "." + std::to_string(rdata[1]) + "." + std::to_string(rdata[2]) +
         "." + std::to_string(rdata[3]);
}

} // namespace

LanMdnsDiscovery::LanMdnsDiscovery() = default;

LanMdnsDiscovery::~LanMdnsDiscovery() {
  Stop();
}

void LanMdnsDiscovery::SetOnDiscovered(DiscoveredFn callback) {
  on_discovered_ = std::move(callback);
}

Roe<std::vector<uint8_t>> LanMdnsDiscovery::EncodeDnsName(const std::string& fqdn) {
  if (fqdn.empty()) {
    return Error("empty dns name");
  }
  std::vector<uint8_t> out;
  size_t start = 0;
  while (start <= fqdn.size()) {
    const size_t dot = fqdn.find('.', start);
    const size_t end = (dot == std::string::npos) ? fqdn.size() : dot;
    const size_t len = end - start;
    if (len > 63) {
      return Error("dns label too long");
    }
    out.push_back(static_cast<uint8_t>(len));
    out.insert(out.end(), fqdn.begin() + static_cast<std::ptrdiff_t>(start),
               fqdn.begin() + static_cast<std::ptrdiff_t>(end));
    if (dot == std::string::npos) {
      break;
    }
    start = dot + 1;
  }
  out.push_back(0);
  return out;
}

Roe<std::string> LanMdnsDiscovery::DecodeDnsName(const std::vector<uint8_t>& packet, size_t offset,
                                                  size_t* out_next) {
  return ReadDnsName(packet, offset, out_next);
}

Roe<void> LanMdnsDiscovery::Start() {
  if (!lan_mdns::UdpMulticastSocket::Init()) {
    return Error("LAN mDNS socket init failed");
  }
  if (running_.load()) {
    return {};
  }
  stop_requested_.store(false);
  running_.store(true);
  thread_ = std::thread([this]() { ThreadMain(); });
  return {};
}

void LanMdnsDiscovery::Stop() {
  stop_requested_.store(true);
  if (thread_.joinable()) {
    thread_.join();
  }
  running_.store(false);
}

void LanMdnsDiscovery::SetAdvertisement(const std::string& peer_id_base58, int tcp_port,
                                        const std::vector<std::string>& lan_ipv4_addrs) {
  std::lock_guard lock(advertise_mutex_);
  advertise_peer_id_ = peer_id_base58;
  advertise_port_ = tcp_port;
  advertise_ips_ = lan_ipv4_addrs;
}

std::optional<std::string> LanMdnsDiscovery::BuildMultiaddr(const LanMdnsDiscoveredPeer& peer) {
  if (peer.peer_id_base58.empty() || peer.host_ip.empty() || peer.tcp_port <= 0) {
    return std::nullopt;
  }
  return "/ip4/" + peer.host_ip + "/tcp/" + std::to_string(peer.tcp_port) + "/p2p/" + peer.peer_id_base58;
}

void LanMdnsDiscovery::ThreadMain() {
  lan_mdns::UdpMulticastSocket socket;
  if (!socket.Open(static_cast<uint16_t>(kMdnsPort), kMdnsMulticast)) {
    running_.store(false);
    return;
  }

  int64_t last_browse_ms = 0;
  int64_t last_announce_ms = 0;

  while (!stop_requested_.load()) {
    const bool readable = socket.WaitReadable(200);

    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();

    if (now_ms - last_browse_ms >= 10000) {
      SendBrowseQuery(socket);
      last_browse_ms = now_ms;
    }

    // Snapshot under lock only — SendAnnouncement locks advertise_mutex_ itself.
    // Holding the lock across that call deadlocks (non-recursive) and freezes the UI
    // on SyncLanMdnsAdvertisement / SetAdvertisement (desktop + mobile tick).
    bool should_announce = false;
    {
      std::lock_guard lock(advertise_mutex_);
      should_announce = !advertise_peer_id_.empty() && advertise_port_ > 0 && !advertise_ips_.empty() &&
                        now_ms - last_announce_ms >= 3000;
    }
    if (should_announce) {
      SendAnnouncement(socket);
      last_announce_ms = now_ms;
    }

    if (readable) {
      uint8_t buf[9000];
      const int n = socket.Recv(buf, sizeof(buf));
      if (n > 0) {
        HandlePacket(buf, static_cast<size_t>(n));
      }
    }
  }

  socket.Close();
  running_.store(false);
}

void LanMdnsDiscovery::SendBrowseQuery(lan_mdns::UdpMulticastSocket& socket) {
  const auto packet = BuildBrowseQuery();
  if (packet.empty()) {
    return;
  }
  socket.Send(packet.data(), packet.size(), static_cast<uint16_t>(kMdnsPort), kMdnsMulticast);
}

void LanMdnsDiscovery::SendAnnouncement(lan_mdns::UdpMulticastSocket& socket) {
  std::string peer_id;
  int port = 0;
  std::vector<std::string> ips;
  {
    std::lock_guard lock(advertise_mutex_);
    peer_id = advertise_peer_id_;
    port = advertise_port_;
    ips = advertise_ips_;
  }
  const auto packet = BuildAnnouncement(peer_id, port, ips);
  if (packet.empty()) {
    return;
  }
  socket.Send(packet.data(), packet.size(), static_cast<uint16_t>(kMdnsPort), kMdnsMulticast);
}

void LanMdnsDiscovery::HandlePacket(const uint8_t* data, size_t len) {
  if (len < 12) {
    return;
  }
  std::vector<uint8_t> packet(data, data + len);
  uint16_t qd = 0;
  uint16_t an = 0;
  uint16_t ns = 0;
  uint16_t ar = 0;
  if (!ReadU16(packet, 4, &qd) || !ReadU16(packet, 6, &an) || !ReadU16(packet, 8, &ns) ||
      !ReadU16(packet, 10, &ar)) {
    return;
  }
  size_t offset = 12;
  for (uint16_t i = 0; i < qd; ++i) {
    auto name = ReadDnsName(packet, offset, &offset);
    if (!name) {
      return;
    }
    offset += 4; // type + class
  }

  std::unordered_map<std::string, int> srv_ports;
  std::unordered_map<std::string, std::string> txt_peers;
  std::unordered_map<std::string, std::string> a_ips;

  const auto parse_records = [&](uint16_t count) {
    for (uint16_t i = 0; i < count; ++i) {
      auto name = ReadDnsName(packet, offset, &offset);
      if (!name) {
        return false;
      }
      uint16_t type = 0;
      uint16_t class_field = 0;
      if (!ReadU16(packet, offset, &type) || !ReadU16(packet, offset + 2, &class_field)) {
        return false;
      }
      offset += 4;
      offset += 4; // ttl
      uint16_t rdlen = 0;
      if (!ReadU16(packet, offset, &rdlen)) {
        return false;
      }
      offset += 2;
      if (offset + rdlen > packet.size()) {
        return false;
      }
      std::vector<uint8_t> rdata(packet.begin() + static_cast<std::ptrdiff_t>(offset),
                                 packet.begin() + static_cast<std::ptrdiff_t>(offset + rdlen));
      offset += rdlen;

      if (*name == kLanMdnsServiceType || name->find("_pp-browser._tcp.local") != std::string::npos ||
          name->find(".local") != std::string::npos) {
        if (type == kDnsTypeSrv) {
          if (auto port = ParseSrvPort(rdata)) {
            srv_ports[*name] = *port;
          }
        } else if (type == kDnsTypeTxt) {
          if (auto peer = ParseTxtPeerId(rdata)) {
            txt_peers[*name] = *peer;
          }
        } else if (type == kDnsTypeA) {
          if (auto ip = ParseARecordIp(rdata)) {
            a_ips[*name] = *ip;
          }
        }
      }
    }
    return true;
  };

  if (!parse_records(an) || !parse_records(ns) || !parse_records(ar)) {
    return;
  }

  for (const auto& [instance, peer_id] : txt_peers) {
    (void)instance;
    LanMdnsDiscoveredPeer discovered;
    discovered.peer_id_base58 = peer_id;
    for (const auto& [name, port] : srv_ports) {
      if (name.find(SanitizeInstanceLabel(peer_id)) != std::string::npos) {
        discovered.tcp_port = port;
        const std::string target = HostTargetFqdn(peer_id);
        if (auto it = a_ips.find(target); it != a_ips.end()) {
          discovered.host_ip = it->second;
        }
        break;
      }
    }
    if (discovered.host_ip.empty()) {
      for (const auto& [host, ip] : a_ips) {
        if (host.find(SanitizeInstanceLabel(peer_id)) != std::string::npos) {
          discovered.host_ip = ip;
          break;
        }
      }
    }
    if (discovered.host_ip.empty() || discovered.tcp_port <= 0) {
      continue;
    }
    if (on_discovered_) {
      on_discovered_(discovered);
    }
  }
}

} // namespace pbr
