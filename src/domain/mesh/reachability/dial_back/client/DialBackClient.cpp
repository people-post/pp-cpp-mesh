#include "domain/mesh/reachability/dial_back/client/DialBackClient.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "common/SettledWait.h"
#include "common/ValueJson.h"
#include "domain/mesh/shared/AmpChannelOpen.h"
#include "domain/mesh/shared/AmpParkUntil.h"

#include <atomic>
#include <chrono>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

std::chrono::milliseconds RemainingTimeout(const Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds(1);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

} // namespace

DialBackClient::DialBackClient(pp::amp::MeshRuntime& runtime, IoPump io_pump)
    : runtime_(runtime), io_pump_(std::move(io_pump)) {}

void DialBackClient::ProbeAsync(const std::string& seed_peer_key,
                                    const std::vector<std::string>& target_multiaddrs,
                                    std::function<void(ProbeRoe)> on_done, int timeout_ms) {
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = std::make_shared<std::function<void(ProbeRoe)>>();
  *finish_once = [on_done = std::move(on_done), settled](ProbeRoe value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (on_done) {
      on_done(std::move(value));
    }
  };

  if (!started_) {
    (*finish_once)(ProbeRoe::error(Failure::Of(Err::NotStarted, "amp dial-back service not started")));
    return;
  }
  if (!runtime_.Links().GetLinkSnapshot(seed_peer_key).has_endpoint) {
    (*finish_once)(ProbeRoe::error(Failure::Of(Err::EndpointNotRegistered, "seed peer endpoint not registered")));
    return;
  }
  if (target_multiaddrs.empty()) {
    (*finish_once)(ProbeRoe::error(Failure::Of(Err::InvalidRequest, "no target_multiaddrs")));
    return;
  }

  Object request;
  request.set("v", int64_t{1});
  request.set("op", "probe");
  std::vector<Value> addrs;
  addrs.reserve(target_multiaddrs.size());
  for (const auto& ma : target_multiaddrs) {
    addrs.emplace_back(ma);
  }
  request.set("target_multiaddrs", ArrayValue(std::move(addrs)));
  request.set("timeout_ms", int64_t{timeout_ms > 0 ? timeout_ms : 8000});
  const std::string request_json = DumpJson(request);

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  auto session = std::make_shared<pp::amp::ChannelSession>();

  // Shared so ChannelSession / PostToIo callbacks can invoke from const contexts.
  auto finish = std::make_shared<std::function<void(ProbeRoe)>>();
  *finish = [finish_once, session](ProbeRoe value) {
    session->Close();
    (*finish_once)(std::move(value));
  };

  const auto read_timeout = RemainingTimeout(deadline);
  runtime_.Links().EnsureAssociation(seed_peer_key, [this, seed_peer_key, request_json, finish, session, deadline,
                                           read_timeout, settled](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
    if (!assoc) {
      (*finish)(ProbeRoe::error(WrapLinkFailure(assoc.error())));
      return;
    }
    runtime_.Links().OpenChannel(seed_peer_key, kDialBackProtocolId, pp::amp::ControlJsonChannelPolicy(read_timeout),
                       [this, seed_peer_key, request_json, finish, session, deadline, read_timeout,
                        settled](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
                         if (!channel) {
                           (*finish)(ProbeRoe::error(WrapLinkFailure(channel.error())));
                           return;
                         }
                         AmpWhenChannelOpen(
                             runtime_.Links(), seed_peer_key, *channel, deadline,
                             [this, seed_peer_key, channel_id = *channel, request_json, finish, session, deadline,
                              read_timeout, settled](bool open) mutable {
                               if (!open) {
                                 (*finish)(ProbeRoe::error(
                                     Failure::Of(Err::ChannelFailed, "amp dial-back: channel open failed")));
                                 return;
                               }
                               auto* link = runtime_.Links().FindLink(seed_peer_key);
                               if (!link || !link->Mux() ||
                                   link->Mux()->State(channel_id) != pp::amp::ChannelState::Open) {
                                 (*finish)(ProbeRoe::error(
                                     Failure::Of(Err::ChannelFailed, "amp dial-back: channel open failed")));
                                 return;
                               }
                               session->Bind(*link->Mux(), channel_id, pp::amp::ControlJsonChannelPolicy(read_timeout),
                                             [finish](Roe<std::vector<uint8_t>> frame) {
                                               if (!frame) {
                                                 (*finish)(ProbeRoe::error(Failure::Of(
                                                     Err::ProtocolError, "Failed to read dial-back response")));
                                                 return false;
                                               }
                                               auto root =
                                                   TryParseObject(std::string(frame->begin(), frame->end()));
                                               if (!root) {
                                                 (*finish)(ProbeRoe::error(Failure::Of(
                                                     Err::ProtocolError, "invalid dial-back response")));
                                                 return false;
                                               }
                                               DialBackProbeResult parsed;
                                               parsed.ok = root->getIf<bool>("ok").value_or(false);
                                               parsed.dialed = root->getString("dialed").value_or("");
                                               parsed.observed = root->getString("observed").value_or("");
                                               parsed.error = root->getString("error").value_or("");
                                               (*finish)(parsed);
                                               return false;
                                             });
                               if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                                 (*finish)(ProbeRoe::error(
                                     Failure::Of(Err::ProtocolError, "Failed to send dial-back probe")));
                                 return;
                               }
                               const auto attempt_ms =
                                   std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                               if (attempt_ms.count() > 0) {
                                 runtime_.PostAfter(attempt_ms, [finish, settled]() {
                                   if (!settled->load(std::memory_order_acquire)) {
                                     (*finish)(ProbeRoe::error(
                                         Failure::Of(Err::Timeout, "dial-back probe timed out")));
                                   }
                                 });
                               }
                             });
                       });
  });
}

DialBackClient::ProbeRoe DialBackClient::Probe(const std::string& seed_peer_key,
                                                         const std::vector<std::string>& target_multiaddrs,
                                                         int timeout_ms) {
  SettledWait<DialBackProbeResult, Failure> wait;
  ProbeAsync(seed_peer_key, target_multiaddrs, [wait](ProbeRoe value) { wait.Finish(std::move(value)); },
             timeout_ms);

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Failure::Of(Err::Timeout, "dial-back probe timed out"));
}

} // namespace pbr
