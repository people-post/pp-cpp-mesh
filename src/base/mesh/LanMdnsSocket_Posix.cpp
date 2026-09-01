#if !defined(_WIN32)

#include "base/mesh/LanMdnsSocket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace pbr::lan_mdns {

struct UdpMulticastSocket::Impl {
  int fd = -1;
};

UdpMulticastSocket::UdpMulticastSocket() : impl_(std::make_unique<Impl>()) {}

UdpMulticastSocket::~UdpMulticastSocket() {
  Close();
}

bool UdpMulticastSocket::Init() {
  return true;
}

bool UdpMulticastSocket::Open(uint16_t port, const char* multicast_ipv4) {
  Close();
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    return false;
  }

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
    close(fd);
    return false;
  }

  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = inet_addr(multicast_ipv4);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

  impl_->fd = fd;
  return true;
}

void UdpMulticastSocket::Close() {
  if (impl_ && impl_->fd >= 0) {
    close(impl_->fd);
    impl_->fd = -1;
  }
}

bool UdpMulticastSocket::IsOpen() const {
  return impl_ && impl_->fd >= 0;
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
  (void)select(impl_->fd + 1, &readfds, nullptr, nullptr, &tv);
  return FD_ISSET(impl_->fd, &readfds) != 0;
}

int UdpMulticastSocket::Recv(uint8_t* buf, size_t cap) {
  if (!IsOpen() || buf == nullptr || cap == 0) {
    return 0;
  }
  const ssize_t n = recvfrom(impl_->fd, buf, cap, 0, nullptr, nullptr);
  return static_cast<int>(n);
}

void UdpMulticastSocket::Send(const uint8_t* data, size_t len, uint16_t dest_port,
                              const char* multicast_ipv4) {
  if (!IsOpen() || data == nullptr || len == 0) {
    return;
  }
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = inet_addr(multicast_ipv4);
  dest.sin_port = htons(dest_port);
  (void)sendto(impl_->fd, data, len, 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}

} // namespace pbr::lan_mdns

#endif // !defined(_WIN32)
