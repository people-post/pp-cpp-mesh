#pragma once

namespace pbr {

/**
 * Amp L4 product protocol ids. Kept apart from each protocol's types so layers below a protocol
 * (reachability, circuit reach) can name the protocols they carry without depending on them.
 */
inline constexpr const char* kDatagramRelayProtocolId = "/pp-browser/datagram-relay/1.0.0";
inline constexpr const char* kMediaRelayProtocolId = kDatagramRelayProtocolId;
inline constexpr const char* kRealtimeProtocolId = "/pp-browser/realtime/1.0.0";
inline constexpr const char* kCallMediaDirectProtocolId = kRealtimeProtocolId;

} // namespace pbr
