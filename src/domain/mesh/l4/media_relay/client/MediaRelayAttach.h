#pragma once

#include "domain/mesh/l4/media_relay/client/IMediaRelayClient.h"
#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
#include "domain/mesh/reachability/MeshReachPorts.h"
#include "foundation/runtime/DeferredSelf.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Mesh ports an attach runs over. `service_reach` may be null (no circuit / punch fallback).
 * `objects_alive` / `objects_snap`: the owner's liveness for these raw pointers (`MeshMediaPlane`
 * invalidates it before it frees them). Service reach can take seconds; the continuation after it
 * (and before AcceptAndAttach) fails instead of dereferencing freed objects. Null = ungated (tests).
 */
struct MediaRelayAttachPorts {
  IMediaRelayClient* relay = nullptr;
  IDialRegistry* dial = nullptr;
  ICircuitHopReach* service_reach = nullptr;
  DeferredSelf::Token objects_alive;
  uint64_t objects_snap = 0;

  bool Live() const { return !objects_alive || DeferredSelf::Alive(objects_alive, objects_snap); }
};

struct MediaRelayAttachRequest {
  std::string hop_peer_id;
  /** Dial hint; registered as the hop's endpoint (and its backoff cleared) when set. */
  std::string hop_multiaddr;
  /** Opaque session id / auth for AcceptAndAttach (a call id today). */
  std::string session_id;
  std::string auth;
  /** Built by the caller (participants, bandwidth wants) — policy, not mechanism. */
  MediaRelayQuoteRequest quote;
};

struct MediaRelayAttachHooks {
  /** Gate a quote before accepting it (e.g. pricing). Empty = accept any ok quote. */
  std::function<Roe<void>(const MediaRelayQuote& quote)> accept_quote;
  /** Checked after the quote, before AcceptAndAttach; false aborts ("attach aborted"). */
  std::function<bool()> still_wanted;
  /** Frame sink handed to AcceptAndAttach — runs on the mesh IO thread; caller decrypts. */
  std::function<void(MediaDataFrame)> on_frame;
};

struct MediaRelayAttached {
  std::string hop_peer_id;
  std::string quote_id;
  /** Granted uplink from the quote (feeds the caller's adaptation). */
  int64_t a_up_bps = 0;
};

/**
 * Attach to a remote `media_relay` hop as a client: make the hop's service dialable (register the
 * hint; service reach when not dialable — media-client-layers L008), quote, gate the quote,
 * AcceptAndAttach. Stateless and feature-neutral: what to do with the attached session
 * (start media, subscribe, fan-out) and how to recover from loss stay with the caller (L009).
 *
 * Threading: continues on an AppRuntime worker after service reach (off the IO strand) and completes
 * on whatever thread the relay client completes on — callers hop to their own thread.
 */
void AttachToMediaRelayAsync(const MediaRelayAttachPorts& ports, MediaRelayAttachRequest request,
                             MediaRelayAttachHooks hooks,
                             std::function<void(Roe<MediaRelayAttached>)> on_done);

/**
 * Quote only — the same reach steps as `AttachToMediaRelayAsync` (register the hint, service reach
 * when not dialable), then a quote, no AcceptAndAttach. A reachability / capacity probe (V050: an
 * invitee checks the planned hop while ringing). Completes on whatever thread the relay client does.
 */
void QuoteMediaRelayAsync(const MediaRelayAttachPorts& ports, MediaRelayAttachRequest request,
                          std::function<void(Roe<MediaRelayQuote>)> on_done);

} // namespace pbr
