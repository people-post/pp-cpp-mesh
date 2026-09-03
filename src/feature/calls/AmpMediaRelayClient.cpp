#include "feature/calls/AmpMediaRelayClient.h"

#include "common/SettledWait.h"

#include <chrono>
#include <thread>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

template <typename T>
auto MakeWaitCallback(const SettledWait<T>& wait) {
  return [wait](Roe<T> result) { wait.Finish(std::move(result)); };
}

} // namespace

AmpMediaRelayClient::AmpMediaRelayClient(AmpMediaRelayCoordinator& coordinator, IoPump io_pump,
                                         std::string local_peer_id)
    : coordinator_(coordinator), io_pump_(std::move(io_pump)), local_peer_id_(std::move(local_peer_id)) {}

Roe<std::string> AmpMediaRelayClient::LocalPeerIdBase58() const {
  if (local_peer_id_.empty()) {
    return Error("amp media-relay: missing local peer id");
  }
  return local_peer_id_;
}

bool AmpMediaRelayClient::IsStarted() const { return coordinator_.IsStarted(); }

void AmpMediaRelayClient::PumpUntil(const std::function<bool()>& done, const int timeout_ms) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);
  while (Clock::now() < deadline) {
    if (done()) {
      return;
    }
    if (io_pump_) {
      io_pump_();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

Roe<MediaRelayQuote> AmpMediaRelayClient::RequestQuote(const std::string& hop_peer_key,
                                                       const MediaRelayQuoteRequest& request,
                                                       const int timeout_ms) {
  if (!IsStarted()) {
    return Error("media-relay not available");
  }
  SettledWait<MediaRelayQuote> wait;
  const auto id = coordinator_.StartQuote(hop_peer_key, request, MakeWaitCallback(wait), timeout_ms);
  if (!id) {
    return Error("media-relay quote not started");
  }
  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  PumpUntil([&wait] { return wait.IsSettled(); }, wait_ms);
  return wait.Wait(std::chrono::milliseconds(wait_ms),
                   Error(std::string("media-relay quote timed out (hop=") + hop_peer_key + ")"));
}

Roe<MediaRelayAttachResult> AmpMediaRelayClient::AcceptAndAttach(
    const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
    const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame, const int timeout_ms) {
  if (!IsStarted()) {
    return Error("media-relay not available");
  }
  SettledWait<MediaRelayAttachResult> wait;
  const auto id = coordinator_.StartAttach(hop_peer_key, quote_id, call_id, auth_stub, std::move(on_frame),
                                           MakeWaitCallback(wait), timeout_ms);
  if (!id) {
    return Error("media-relay attach not started");
  }
  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  PumpUntil([&wait] { return wait.IsSettled(); }, wait_ms);
  return wait.Wait(std::chrono::milliseconds(wait_ms),
                   Error(std::string("media-relay attach timed out (hop=") + hop_peer_key + ")"));
}

void AmpMediaRelayClient::StartClientFrameReader() { coordinator_.StartClientFrameReader(); }

void AmpMediaRelayClient::SetClientTransportLostHandler(std::function<void()> handler) {
  coordinator_.SetClientTransportLostHandler(std::move(handler));
}

Roe<MediaRelayAttachResult> AmpMediaRelayClient::AttachAsLocalHop(
    const std::string& call_id, std::function<void(MediaDataFrame)> on_frame) {
  return coordinator_.AttachAsLocalHop(call_id, std::move(on_frame));
}

Roe<void> AmpMediaRelayClient::Subscribe(const uint32_t stream_id, const uint16_t channel_id) {
  return coordinator_.Subscribe(stream_id, channel_id);
}

Roe<void> AmpMediaRelayClient::SendFrame(const MediaDataFrame& frame) {
  return coordinator_.SendFrame(frame);
}

void AmpMediaRelayClient::Detach() { coordinator_.Detach(); }

bool AmpMediaRelayClient::IsAttached() const { return coordinator_.IsAttached(); }

bool AmpMediaRelayClient::IsLocalHopAttached() const { return coordinator_.IsLocalHopAttached(); }

double AmpMediaRelayClient::PathPressure() const { return coordinator_.PathPressure(); }

CallHopHealth AmpMediaRelayClient::HealthSnapshot() const { return coordinator_.HealthSnapshot(); }

} // namespace pbr
