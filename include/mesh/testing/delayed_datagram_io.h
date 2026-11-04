#pragma once

#include "amp/L1/Clock.h"
#include "amp/L1/DatagramIo.h"

#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace pp::mesh::test {

/**
 * A DatagramIo whose received datagrams arrive `delay_ms` after they reached it, on `clock`: wrapped around a
 * node's IO, every exchange with it takes that much longer (a far node — an ocean away, a slow mobile path).
 * Sends go out at once. Single-threaded, like the IO it wraps.
 */
class DelayedDatagramIo final : public pp::adp::DatagramIo {
public:
  DelayedDatagramIo(std::shared_ptr<pp::adp::DatagramIo> inner, std::shared_ptr<pp::adp::Clock> clock,
                    int64_t delay_ms);

  pp::Roe<void> SendTo(const pp::adp::IpEndpoint& peer, std::span<const uint8_t> datagram) override;
  pp::Roe<std::optional<std::pair<pp::adp::IpEndpoint, std::vector<uint8_t>>>> RecvFrom() override;
  pp::adp::IpEndpoint LocalEndpoint() const override { return inner_->LocalEndpoint(); }

private:
  struct Held {
    int64_t due_ms = 0;
    pp::adp::IpEndpoint from;
    std::vector<uint8_t> datagram;
  };
  std::shared_ptr<pp::adp::DatagramIo> inner_;
  std::shared_ptr<pp::adp::Clock> clock_;
  int64_t delay_ms_;
  std::deque<Held> held_;
};

} // namespace pp::mesh::test
