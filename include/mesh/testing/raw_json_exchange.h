#pragma once

#include "amp/L3/ChannelPolicy.h"
#include "amp/link/PeerLinkManager.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pp::mesh::test {

/** What a server answered a hand-written exchange (`RawJsonExchange`). */
struct RawJsonReplies {
  /** The channel opened (false: the peer refused it, or it did not open in time). */
  bool opened = false;
  /** The server's frames, in order. */
  std::vector<std::string> frames;
  /** The server ended the channel. */
  bool closed = false;
};

/**
 * Speak a JSON protocol by hand, as an older or a newer peer would: open `protocol_id` to `peer_key` on `links`, send
 * `requests` in order, and collect what the server answers until `want_frames` frames came, the channel ended, or
 * `max_rounds` of `pump` ran. For wire-compatibility tests.
 */
RawJsonReplies RawJsonExchange(pp::amp::PeerLinkManager& links, const std::function<void()>& pump,
                               const std::string& peer_key, const std::string& protocol_id,
                               const std::vector<std::string>& requests, size_t want_frames = 1,
                               size_t max_rounds = 2000,
                               pp::amp::ChannelPolicy policy = pp::amp::ControlJsonChannelPolicy());

} // namespace pp::mesh::test
