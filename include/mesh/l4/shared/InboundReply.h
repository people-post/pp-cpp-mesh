#pragma once

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"

#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace pbr {

/**
 * Reply side of an inbound request channel whose handler runs off the Amp IO strand.
 *
 * `ChannelSession` is IO-affine, and a frame handler that returns `false` closes the session as
 * soon as it returns — a worker's later `EnqueueOutbound` then fails. Under MeshPump (product,
 * pp-call-probe) every Amp direct-chat ack was lost that way and each send fell back to the relay
 * after 4 s (2026-09-25). Pattern for L4 request handlers that post to a worker:
 *
 *   bind:          `InboundReplyPolicy(policy)` (no read_once)
 *   frame handler: `auto reply = MakeInboundReply(session, post_io);` → post work → `return true`
 *   worker:        `reply->Send(bytes)` (any number) — hops onto IO in order
 *   done:          the last owner drops `reply` → Close on IO (no exit path leaks the channel)
 *
 * With an empty `post_io` (inline test harnesses) Send / Close run on the caller.
 */
class InboundReply {
public:
  using IoPost = std::function<void(std::function<void()>)>;

  InboundReply(std::shared_ptr<pp::amp::ChannelSession> session, IoPost post_io)
      : session_(std::move(session)), post_io_(std::move(post_io)) {}
  ~InboundReply() { Close(); }

  InboundReply(const InboundReply&) = delete;
  InboundReply& operator=(const InboundReply&) = delete;

  void Send(std::vector<uint8_t> body) {
    RunOnIo([session = session_, body = std::move(body)]() mutable {
      if (session) {
        (void)session->EnqueueOutbound(std::move(body));
      }
    });
  }

  /** Close after every queued Send (IO lane is FIFO). Idempotent; also runs on destruction. */
  void Close() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    RunOnIo([session = session_]() {
      if (session && !session->IsClosed()) {
        session->Close();
      }
    });
  }

private:
  void RunOnIo(std::function<void()> task) {
    if (post_io_) {
      post_io_(std::move(task));
    } else {
      task();
    }
  }

  std::shared_ptr<pp::amp::ChannelSession> session_;
  IoPost post_io_;
  std::atomic<bool> closed_{false};
};

/**
 * Inbound policy for a request answered by `InboundReply`: `read_once` would close the channel
 * right after the request frame, before the worker replies. `read_timeout` still bounds it.
 */
inline pp::amp::ChannelPolicy InboundReplyPolicy(pp::amp::ChannelPolicy policy) {
  policy.read_once = false;
  return policy;
}

inline std::shared_ptr<InboundReply> MakeInboundReply(std::shared_ptr<pp::amp::ChannelSession> session,
                                                      InboundReply::IoPost post_io) {
  return std::make_shared<InboundReply>(std::move(session), std::move(post_io));
}

} // namespace pbr
