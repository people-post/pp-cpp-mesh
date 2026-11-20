#include "mesh/testing/raw_json_exchange.h"

#include "amp/L3/ChannelSession.h"

#include <chrono>
#include <memory>
#include <optional>

namespace pp::mesh::test {

RawJsonReplies RawJsonExchange(pp::amp::PeerLinkManager& links, const std::function<void()>& pump,
                               const std::string& peer_key, const std::string& protocol_id,
                               const std::vector<std::string>& requests, const size_t want_frames,
                               const size_t max_rounds, pp::amp::ChannelPolicy policy) {
  struct State {
    std::optional<uint32_t> channel_id;
    bool open_failed = false;
    std::optional<bool> open;
    RawJsonReplies replies;
  };
  auto state = std::make_shared<State>();
  policy.read_once = false;
  size_t rounds = 0;
  const auto pump_until = [&](const std::function<bool()>& done) {
    for (; rounds < max_rounds && !done(); ++rounds) {
      pump();
    }
  };

  links.EnsureAssociation(
      peer_key, [&links, state, peer_key, protocol_id, policy](pp::amp::PeerLinkManager::LinkRoe assoc) {
        if (!assoc) {
          state->open_failed = true;
          return;
        }
        links.OpenChannel(peer_key, protocol_id, policy, [state](const pp::amp::PeerLinkManager::ChannelRoe& channel) {
          if (!channel) {
            state->open_failed = true;
            return;
          }
          state->channel_id = *channel;
        });
      });
  pump_until([&] { return state->channel_id.has_value() || state->open_failed; });
  if (!state->channel_id) {
    return state->replies;
  }
  links.WhenChannelOpen(peer_key, *state->channel_id, links.GetEndpoint().GetClock().NowMs() + 10000,
                        [state](const bool open) { state->open = open; });
  pump_until([&] { return state->open.has_value(); });
  if (!state->open.value_or(false)) {
    return state->replies;
  }
  state->replies.opened = true;
  auto session = links.BindChannel(
      peer_key, *state->channel_id, policy,
      [state](Roe<std::vector<uint8_t>> frame) {
        if (!frame) {
          state->replies.closed = true;
          return false;
        }
        state->replies.frames.emplace_back(frame->begin(), frame->end());
        return true;
      },
      [state](const char*) { state->replies.closed = true; });
  if (!session) {
    return state->replies;
  }
  for (const std::string& request : requests) {
    (void)session->EnqueueOutbound(std::vector<uint8_t>(request.begin(), request.end()));
  }
  pump_until([&] { return state->replies.frames.size() >= want_frames || state->replies.closed; });
  RawJsonReplies out = state->replies;
  session->Close();
  return out;
}

} // namespace pp::mesh::test
