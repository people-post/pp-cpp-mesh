#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pbr::lan_mdns {

/** UDP IPv4 multicast socket for LAN mDNS (port 5353 / 224.0.0.251). */
class UdpMulticastSocket {
public:
  UdpMulticastSocket();
  ~UdpMulticastSocket();

  UdpMulticastSocket(const UdpMulticastSocket&) = delete;
  UdpMulticastSocket& operator=(const UdpMulticastSocket&) = delete;

  /** WSAStartup on Win32; always true on POSIX. */
  static bool Init();

  bool Open(uint16_t port, const char* multicast_ipv4);
  void Close();
  bool IsOpen() const;

  /** Wait up to timeout_ms for a readable datagram. */
  bool WaitReadable(int timeout_ms);

  /** Bytes received, or <= 0 on error / empty. */
  int Recv(uint8_t* buf, size_t cap);

  void Send(const uint8_t* data, size_t len, uint16_t dest_port, const char* multicast_ipv4);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace pbr::lan_mdns
