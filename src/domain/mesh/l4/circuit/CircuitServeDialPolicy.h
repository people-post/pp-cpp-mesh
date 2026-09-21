#pragma once

#include <cstddef>
#include <string_view>

namespace pbr {

/**
 * H010 dual-NAT ServeDial / nested call-media policy (single home).
 *
 * Same failure used to show up as dialer `not registered`, WaitAck `bridge timed out`,
 * or answerer punch give-up — all from cold-dialing a private Preferred or ServeDial
 * without a live Connected far leg. Keep the rules here; call sites only apply them.
 */

/** Peer-id-only far leg: only a live Connected PeerLink (inbound alias ok). Never Preferred. */
inline bool CircuitPeerIdOnlyHasLiveFarLeg(const std::size_t connected_links_for_peer_id) {
  return connected_links_for_peer_id > 0;
}

inline constexpr std::string_view kCircuitTargetPeerNotRegistered =
    "circuit target peer endpoint not registered";

/**
 * When the hop holds a live op=reserve for the target, clear an explicit target_multiaddr
 * so ServeDial takes the peer-id-only Connected path (avoids RegisterEndpoint private MA).
 */
inline bool CircuitServeDialClearTargetMaWhenReserved(const bool has_live_reservation,
                                                     const bool target_ma_nonempty) {
  return has_live_reservation && target_ma_nonempty;
}

/**
 * Peer-id-only resolution succeeded (empty MA) → open call-media on the live link.
 * Do not EnsureAssociation(PeerId) — that can dial dial-book Preferred into NAT.
 */
inline bool CircuitServeDialOpenOnLiveLink(const bool resolved_multiaddr_empty) {
  return resolved_multiaddr_empty;
}

/**
 * After answerer seed park, skip EnsureAssociation of a non-public Preferred to the call
 * peer — that UDP dial can drop the hop PeerLink (dogfood 39412f).
 */
inline bool CallMediaShouldSkipPreferredDialAfterSeedPark(const bool seed_park_ok,
                                                         const bool dial_public_direct) {
  return seed_park_ok && !dial_public_direct;
}

} // namespace pbr
