#pragma once

namespace pbr {

/**
 * The mesh's own Amp L4 protocol ids. Kept apart from each protocol's types so layers below a
 * protocol (reachability, circuit reach) can name the protocols they carry without depending on
 * them. The app's protocols are its own (MeshHost::SetAppProtocols).
 */
inline constexpr const char* kCircuitProtocolId = "/pp-browser/circuit/1.0.0";
inline constexpr const char* kCircuitRelayProtocolId = kCircuitProtocolId;
inline constexpr const char* kDatagramRelayProtocolId = "/pp-browser/datagram-relay/1.0.0";
inline constexpr const char* kMediaRelayProtocolId = kDatagramRelayProtocolId;

} // namespace pbr
