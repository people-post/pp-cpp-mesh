#include "mesh/l4/media_relay/client/AmpMediaRelayClient.h"

#include "common/SettledWait.h"
#include "mesh/shared/AmpParkUntil.h"

#include <atomic>
#include <chrono>
#include <memory>
namespace pp::mesh {
namespace {

using Clock = std::chrono::steady_clock;
using Err = MediaRelayErr;
using Failure = MediaRelayFailure;

Failure TimedOut(const char* what, const std::string& hop_peer_key) {
  return Failure::Of(Err::Timeout, std::string("media-relay ") + what + " timed out (hop=" + hop_peer_key + ")");
}

} // namespace

AmpMediaRelayClient::AmpMediaRelayClient(MediaRelayClientCoordinator& coordinator, IoPump io_pump,
                                         std::string local_peer_id, IoPost post_io, IoAfter post_after)
    : coordinator_(coordinator), io_pump_(std::move(io_pump)), post_io_(std::move(post_io)),
      post_after_(std::move(post_after)), local_peer_id_(std::move(local_peer_id)) {}

Roe<std::string> AmpMediaRelayClient::LocalPeerIdBase58() const {
  if (local_peer_id_.empty()) {
    return Error("amp media-relay: missing local peer id");
  }
  return local_peer_id_;
}

bool AmpMediaRelayClient::IsStarted() const { return coordinator_.IsStarted(); }

void AmpMediaRelayClient::RequestQuoteAsync(const std::string& hop_peer_key,
                                            const MediaRelayQuoteRequest& request,
                                            std::function<void(MediaRelayRoe<MediaRelayQuote>)> on_done,
                                            const int timeout_ms) {
  if (!on_done) {
    return;
  }
  if (!IsStarted()) {
    on_done(Failure::Of(Err::NotStarted, "media-relay not available"));
    return;
  }

  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = std::make_shared<std::function<void(MediaRelayRoe<MediaRelayQuote>)>>();
  *finish_once = [on_done = std::move(on_done), settled](MediaRelayRoe<MediaRelayQuote> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    on_done(std::move(value));
  };

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  const auto id = coordinator_.StartQuote(
      hop_peer_key, request,
      [finish_once](MediaRelayRoe<MediaRelayQuote> result) { (*finish_once)(std::move(result)); }, timeout_ms);
  if (!id) {
    return;  // refused: the coordinator posts why
  }
  AmpScheduleUntilSettled(post_io_, io_pump_, settled, deadline, [finish_once, hop_peer_key]() {
    (*finish_once)(TimedOut("quote", hop_peer_key));
  },
                          post_after_);
}

MediaRelayRoe<MediaRelayQuote> AmpMediaRelayClient::RequestQuote(const std::string& hop_peer_key,
                                                       const MediaRelayQuoteRequest& request,
                                                       const int timeout_ms) {
  SettledWait<MediaRelayQuote, Failure> wait;
  RequestQuoteAsync(hop_peer_key, request, [wait](MediaRelayRoe<MediaRelayQuote> result) { wait.Finish(std::move(result)); },
                    timeout_ms);
  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), TimedOut("quote", hop_peer_key));
}

void AmpMediaRelayClient::AcceptAndAttachAsync(const std::string& hop_peer_key, const std::string& quote_id,
                                               const std::string& session_id, const std::string& auth_stub,
                                               std::function<void(MediaDataFrame)> on_frame,
                                               std::function<void(MediaRelayRoe<MediaRelayAttachResult>)> on_done,
                                               const int timeout_ms) {
  if (!on_done) {
    return;
  }
  if (!IsStarted()) {
    on_done(Failure::Of(Err::NotStarted, "media-relay not available"));
    return;
  }

  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = std::make_shared<std::function<void(MediaRelayRoe<MediaRelayAttachResult>)>>();
  *finish_once = [on_done = std::move(on_done), settled](MediaRelayRoe<MediaRelayAttachResult> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    on_done(std::move(value));
  };

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  const auto id = coordinator_.StartAttach(
      hop_peer_key, quote_id, session_id, auth_stub, std::move(on_frame),
      [finish_once](MediaRelayRoe<MediaRelayAttachResult> result) { (*finish_once)(std::move(result)); }, timeout_ms);
  if (!id) {
    return;  // refused: the coordinator posts why
  }
  AmpScheduleUntilSettled(post_io_, io_pump_, settled, deadline, [finish_once, hop_peer_key]() {
    (*finish_once)(TimedOut("attach", hop_peer_key));
  },
                          post_after_);
}

MediaRelayRoe<MediaRelayAttachResult> AmpMediaRelayClient::AcceptAndAttach(
    const std::string& hop_peer_key, const std::string& quote_id, const std::string& session_id,
    const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame, const int timeout_ms) {
  SettledWait<MediaRelayAttachResult, Failure> wait;
  AcceptAndAttachAsync(hop_peer_key, quote_id, session_id, auth_stub, std::move(on_frame),
                       [wait](MediaRelayRoe<MediaRelayAttachResult> result) { wait.Finish(std::move(result)); }, timeout_ms);
  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), TimedOut("attach", hop_peer_key));
}

void AmpMediaRelayClient::StartClientFrameReader() { coordinator_.StartClientFrameReader(); }

uint64_t AmpMediaRelayClient::AddClientTransportLostObserver(std::function<void(MediaRelayClientLoss)> observer) {
  return coordinator_.AddClientTransportLostObserver(std::move(observer));
}

void AmpMediaRelayClient::RemoveClientTransportLostObserver(uint64_t token) {
  coordinator_.RemoveClientTransportLostObserver(token);
}

MediaRelayRoe<MediaRelayAttachResult> AmpMediaRelayClient::AttachAsLocalHop(
    const std::string& session_id, std::function<void(MediaDataFrame)> on_frame) {
  return coordinator_.AttachAsLocalHop(session_id, std::move(on_frame));
}

MediaRelayRoe<void> AmpMediaRelayClient::Subscribe(const uint32_t stream_id, const uint16_t channel_id) {
  return coordinator_.Subscribe(stream_id, channel_id);
}

MediaRelayRoe<void> AmpMediaRelayClient::SendFrame(const MediaDataFrame& frame) {
  return coordinator_.SendFrame(frame);
}

void AmpMediaRelayClient::Detach() { coordinator_.Detach(); }

bool AmpMediaRelayClient::IsAttached() const { return coordinator_.IsAttached(); }

bool AmpMediaRelayClient::IsLocalHopAttached() const { return coordinator_.IsLocalHopAttached(); }

double AmpMediaRelayClient::PathPressure() const { return coordinator_.PathPressure(); }

MediaRelayClientHealth AmpMediaRelayClient::HealthSnapshot() const { return coordinator_.HealthSnapshot(); }

} // namespace pp::mesh
