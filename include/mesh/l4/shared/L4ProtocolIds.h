#pragma once

namespace pp::mesh {

/**
 * The mesh's own Amp L4 protocol ids. Kept apart from each protocol's types so layers below a
 * protocol (reachability, circuit reach) can name the protocols they carry without depending on
 * them. The app's protocols are its own (MeshHost::SetAppProtocols).
 */
inline constexpr const char* kCircuitProtocolId = "/pp-mesh/circuit/1.0.0";
inline constexpr const char* kCircuitRelayProtocolId = kCircuitProtocolId;
inline constexpr const char* kDatagramRelayProtocolId = "/pp-mesh/datagram-relay/1.0.0";
inline constexpr const char* kMediaRelayProtocolId = kDatagramRelayProtocolId;
/**
 * A media-relay participant's data lane: media frames to and from the hop, best
 * effort (MediaRelayDataChannelPolicy), beside its control channel
 * (kMediaRelayProtocolId, reliable: quote, attach, subscribe — never media). Both
 * ride the same link, direct or nested through a circuit.
 */
inline constexpr const char* kMediaRelayDataProtocolId = "/pp-mesh/media-relay-data/1.0.0";

} // namespace pp::mesh
