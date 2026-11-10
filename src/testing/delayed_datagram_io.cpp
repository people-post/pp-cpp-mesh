#include "mesh/testing/delayed_datagram_io.h"

namespace pp::mesh::test {

DelayedDatagramIo::DelayedDatagramIo(std::shared_ptr<pp::adp::DatagramIo> inner, std::shared_ptr<pp::adp::Clock> clock,
                                     const int64_t delay_ms)
    : inner_(std::move(inner)), clock_(std::move(clock)), delay_ms_(delay_ms) {}

pp::Roe<void> DelayedDatagramIo::SendTo(const pp::adp::IpEndpoint& peer, const std::span<const uint8_t> datagram) {
  return inner_->SendTo(peer, datagram);
}

pp::Roe<std::optional<std::pair<pp::adp::IpEndpoint, std::vector<uint8_t>>>> DelayedDatagramIo::RecvFrom() {
  const int64_t now = clock_->NowMs();
  // Take everything that reached us; it arrives delay_ms from now, in order.
  for (;;) {
    auto got = inner_->RecvFrom();
    if (!got) {
      return got.error();
    }
    if (!got->has_value()) {
      break;
    }
    held_.push_back(Held{now + delay_ms_, std::move((*got)->first), std::move((*got)->second)});
  }
  if (held_.empty() || held_.front().due_ms > now) {
    return std::optional<std::pair<pp::adp::IpEndpoint, std::vector<uint8_t>>>{};
  }
  Held next = std::move(held_.front());
  held_.pop_front();
  return std::optional<std::pair<pp::adp::IpEndpoint, std::vector<uint8_t>>>{
      std::make_pair(std::move(next.from), std::move(next.datagram))};
}

} // namespace pp::mesh::test
