#if defined(_WIN32)

#include "base/mesh/reachability/LanMdnsSocket.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

namespace pbr::lan_mdns {
namespace {

bool EnsureWinsock() {
  static bool ready = false;
  static bool ok = false;
  if (ready) {
    return ok;
  }
  WSADATA data{};
  ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
  ready = true;
  return ok;
}

} // namespace

struct UdpMulticastSocket::Impl {
  SOCKET fd = INVALID_SOCKET;
};

UdpMulticastSocket::UdpMulticastSocket() : impl_(std::make_unique<Impl>()) {}

UdpMulticastSocket::~UdpMulticastSocket() {
  Close();
}

bool UdpMulticastSocket::Init() {
  return EnsureWinsock();
}

bool UdpMulticastSocket::Open(uint16_t port, const char* multicast_ipv4) {
  Close();
  if (!EnsureWinsock()) {
    return false;
  }
  SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == INVALID_SOCKET) {
    return false;
  }

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
    closesocket(fd);
    return false;
  }

  ip_mreq mreq{};
  InetPtonA(AF_INET, multicast_ipv4, &mreq.imr_multiaddr);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

  impl_->fd = fd;
  return true;
}

void UdpMulticastSocket::Close() {
  if (impl_ && impl_->fd != INVALID_SOCKET) {
    closesocket(impl_->fd);
    impl_->fd = INVALID_SOCKET;
  }
}

bool UdpMulticastSocket::IsOpen() const {
  return impl_ && impl_->fd != INVALID_SOCKET;
}

bool UdpMulticastSocket::WaitReadable(int timeout_ms) {
  if (!IsOpen()) {
    return false;
  }
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(impl_->fd, &readfds);
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  (void)select(0, &readfds, nullptr, nullptr, &tv);
  return FD_ISSET(impl_->fd, &readfds) != 0;
}

int UdpMulticastSocket::Recv(uint8_t* buf, size_t cap) {
  if (!IsOpen() || buf == nullptr || cap == 0) {
    return 0;
  }
  const int n = recvfrom(impl_->fd, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0, nullptr,
                         nullptr);
  return n;
}

void UdpMulticastSocket::Send(const uint8_t* data, size_t len, uint16_t dest_port,
                              const char* multicast_ipv4) {
  if (!IsOpen() || data == nullptr || len == 0) {
    return;
  }
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  InetPtonA(AF_INET, multicast_ipv4, &dest.sin_addr);
  dest.sin_port = htons(dest_port);
  (void)sendto(impl_->fd, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
               reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}

} // namespace pbr::lan_mdns

#endif // defined(_WIN32)
