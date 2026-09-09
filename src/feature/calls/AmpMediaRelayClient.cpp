#include "feature/calls/AmpMediaRelayClient.h"

#include "common/SettledWait.h"
#include "domain/mesh/shared/AmpParkUntil.h"

#include <atomic>
#include <chrono>
#include <memory>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

} // namespace

AmpMediaRelayClient::AmpMediaRelayClient(AmpMediaRelayCoordinator& coordinator, IoPump io_pump,
                                         std::string local_peer_id, IoPost post_io)
    : coordinator_(coordinator), io_pump_(std::move(io_pump)), post_io_(std::move(post_io)),
      local_peer_id_(std::move(local_peer_id)) {}

Roe<std::string> AmpMediaRelayClient::LocalPeerIdBase58() const {
  if (local_peer_id_.empty()) {
    return Error("amp media-relay: missing local peer id");
  }
  return local_peer_id_;
}

bool AmpMediaRelayClient::IsStarted() const { return coordinator_.IsStarted(); }

void AmpMediaRelayClient::RequestQuoteAsync(const std::string& hop_peer_key,
                                            const MediaRelayQuoteRequest& request,
                                            std::function<void(Roe<MediaRelayQuote>)> on_done,
                                            const int timeout_ms) {
  if (!on_done) {
    return;
  }
  if (!IsStarted()) {
    on_done(Error("media-relay not available"));
    return;
  }

  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = std::make_shared<std::function<void(Roe<MediaRelayQuote>)>>();
  *finish_once = [on_done = std::move(on_done), settled](Roe<MediaRelayQuote> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    on_done(std::move(value));
  };

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  const auto id = coordinator_.StartQuote(
      hop_peer_key, request,
      [finish_once](Roe<MediaRelayQuote> result) { (*finish_once)(std::move(result)); }, timeout_ms);
  if (!id) {
    (*finish_once)(Error("media-relay quote not started"));
    return;
  }
  AmpScheduleUntilSettled(post_io_, io_pump_, settled, deadline, [finish_once, hop_peer_key]() {
    (*finish_once)(Error(std::string("media-relay quote timed out (hop=") + hop_peer_key + ")"));
  });
}

Roe<MediaRelayQuote> AmpMediaRelayClient::RequestQuote(const std::string& hop_peer_key,
                                                       const MediaRelayQuoteRequest& request,
                                                       const int timeout_ms) {
  SettledWait<MediaRelayQuote> wait;
  RequestQuoteAsync(hop_peer_key, request, [wait](Roe<MediaRelayQuote> result) { wait.Finish(std::move(result)); },
                    timeout_ms);
  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1),
                   Error(std::string("media-relay quote timed out (hop=") + hop_peer_key + ")"));
}

void AmpMediaRelayClient::AcceptAndAttachAsync(const std::string& hop_peer_key, const std::string& quote_id,
                                               const std::string& call_id, const std::string& auth_stub,
                                               std::function<void(MediaDataFrame)> on_frame,
                                               std::function<void(Roe<MediaRelayAttachResult>)> on_done,
                                               const int timeout_ms) {
  if (!on_done) {
    return;
  }
  if (!IsStarted()) {
    on_done(Error("media-relay not available"));
    return;
  }

  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = std::make_shared<std::function<void(Roe<MediaRelayAttachResult>)>>();
  *finish_once = [on_done = std::move(on_done), settled](Roe<MediaRelayAttachResult> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    on_done(std::move(value));
  };

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  const auto id = coordinator_.StartAttach(
      hop_peer_key, quote_id, call_id, auth_stub, std::move(on_frame),
      [finish_once](Roe<MediaRelayAttachResult> result) { (*finish_once)(std::move(result)); }, timeout_ms);
  if (!id) {
    (*finish_once)(Error("media-relay attach not started"));
    return;
  }
  AmpScheduleUntilSettled(post_io_, io_pump_, settled, deadline, [finish_once, hop_peer_key]() {
    (*finish_once)(Error(std::string("media-relay attach timed out (hop=") + hop_peer_key + ")"));
  });
}

Roe<MediaRelayAttachResult> AmpMediaRelayClient::AcceptAndAttach(
    const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
    const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame, const int timeout_ms) {
  SettledWait<MediaRelayAttachResult> wait;
  AcceptAndAttachAsync(hop_peer_key, quote_id, call_id, auth_stub, std::move(on_frame),
                       [wait](Roe<MediaRelayAttachResult> result) { wait.Finish(std::move(result)); }, timeout_ms);
  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1),
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
