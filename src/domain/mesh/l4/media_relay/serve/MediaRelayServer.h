#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
#include "domain/mesh/l4/media_relay/MediaRelayVideoLevels.h"
#include "domain/mesh/l4/shared/RelayRuntimeStats.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace pbr {

/**
 * Serving side of `/pp-browser/datagram-relay/1.0.0` on MeshRuntime ([A022]): answers inbound
 * quote / accept / attach under the admission policy, keeps the quote book, and fans frames out
 * between the participants of each hosted session. MeshHost owns one when Amp is up; the protocol
 * handler is always registered and `SetServeInbound(false)` refuses new work.
 *
 * This node's own client can join a hosted session without dialing itself (the local hop):
 * `MediaRelayClientCoordinator` drives the `*Local` calls. Lock order: client → server.
 */
class MediaRelayServer {
public:
  using FrameHandler = std::function<void(MediaDataFrame)>;

  explicit MediaRelayServer(pp::amp::MeshRuntime& runtime);
  ~MediaRelayServer();

  MediaRelayServer(const MediaRelayServer&) = delete;
  MediaRelayServer& operator=(const MediaRelayServer&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const;

  /** When false, the inbound protocol handler refuses new quotes / attaches. */
  void SetServeInbound(bool serve);
  bool ServeInbound() const;
  void SetAdmissionPolicy(MediaRelayAdmissionPolicy policy);
  /** Hosted sessions with participants, and those participants (aggregates only). Any thread. */
  MediaRelayRuntimeStats RuntimeStats() const;
  /** B009: which video levels this relay carries per publisher (from the operator's config). */
  void SetVideoPolicy(MediaRelayVideoPolicy policy);
  /** Test: how long a session outlives its last participant (default kEmptySessionGrace). */
  void SetEmptySessionGraceForTest(std::chrono::milliseconds grace);

  /** Drop every hosted session, pending quote and the local participant. */
  void AbortInflight();

  // --- local participant (this node's client on its own hop) --------------------------------
  /** Join (or create) the hosted session for `call_id`; re-attaching the same call swaps the handler. */
  Roe<MediaRelayAttachResult> AttachLocal(const std::string& call_id, const std::string& local_peer_id,
                                          FrameHandler on_frame);
  /** True when the local participant is in `call_id` as `local_peer_id`. */
  bool LocalAttachedTo(const std::string& call_id, const std::string& local_peer_id) const;
  void DetachLocal();
  bool IsLocalAttached() const;
  /** False when there is no local participant. */
  bool SubscribeLocal(uint32_t stream_id, uint16_t channel_id);
  /** Fan a frame out from the local participant; false when there is none. */
  bool SendLocal(const MediaDataFrame& frame);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
};

} // namespace pbr
