# Product Amp mesh glue (domain peer). Libp2p Host/Yamux/Noise L4 retired from product (A017/D10).
# PeerId / identity lives under src/foundation/identity/ (pp_foundation_identity).

set(PP_DOMAIN_MESH_HOST_SOURCES
  host/MeshHost.cpp
  host/MeshPorts.cpp
)

set(PP_DOMAIN_MESH_REACHABILITY_SOURCES
  reachability/Reachability.cpp
  reachability/ReachabilityEngine.cpp
  reachability/NatTraversal.cpp
  reachability/LanMdnsDiscovery.cpp
  reachability/AmpDialBackProtocol.cpp
  reachability/AmpObservedAddrs.cpp
  reachability/PunchLogic.cpp
  reachability/AmpPunchCoordinator.cpp
  reachability/MobileEphemeralListenGate.cpp
)

set(PP_DOMAIN_MESH_DISCOVERY_SOURCES
  discovery/MeshDirectoryCache.cpp
  discovery/NameDirectory.cpp
  discovery/MeshNodeHitCodec.cpp
  discovery/AmpDirectoryProtocol.cpp
)

set(PP_DOMAIN_MESH_DHT_SOURCES
  dht/DhtRecordCodec.cpp
  dht/DhtRecordStore.cpp
  dht/DhtRateLimiter.cpp
  dht/AmpDhtProtocol.cpp
)

set(PP_DOMAIN_MESH_L4_CIRCUIT_SOURCES
  l4/circuit/AmpCircuitHopRegistry.cpp
  l4/circuit/CircuitBundleLogic.cpp
  l4/circuit/CircuitTunnelCoordinator.cpp
)

set(PP_DOMAIN_MESH_L4_MEDIA_RELAY_SOURCES
  l4/media_relay/MediaRelayFrames.cpp
  l4/media_relay/MediaRelayAttachSm.cpp
  l4/media_relay/MediaRelayLogic.cpp
  l4/media_relay/MediaRelayBundleLogic.cpp
  l4/media_relay/AmpMediaRelayCoordinator.cpp
)

set(PP_DOMAIN_MESH_L4_CALL_MEDIA_SOURCES
  l4/call_media/CallMediaLegCoordinator.cpp
  l4/call_media/CallMediaAmpTransport.cpp
  l4/call_media/CallMediaBundleLogic.cpp
  l4/call_media/CallMediaSessionLogic.cpp
  l4/call_media/CallMediaFrameCrypto.cpp
)

if(WIN32)
  list(APPEND PP_DOMAIN_MESH_REACHABILITY_SOURCES
    reachability/LanMdnsSocket_Win32.cpp
    reachability/ReachabilityNetIf_Win32.cpp)
else()
  list(APPEND PP_DOMAIN_MESH_REACHABILITY_SOURCES
    reachability/LanMdnsSocket_Posix.cpp
    reachability/ReachabilityNetIf_Posix.cpp)
endif()

pp_browser_add_domain_library(pp_domain_mesh
  SOURCES
    ${PP_DOMAIN_MESH_HOST_SOURCES}
    ${PP_DOMAIN_MESH_REACHABILITY_SOURCES}
    ${PP_DOMAIN_MESH_DISCOVERY_SOURCES}
    ${PP_DOMAIN_MESH_DHT_SOURCES}
    ${PP_DOMAIN_MESH_L4_CIRCUIT_SOURCES}
    ${PP_DOMAIN_MESH_L4_MEDIA_RELAY_SOURCES}
    ${PP_DOMAIN_MESH_L4_CALL_MEDIA_SOURCES}
  PUBLIC_LIBS
    pp_foundation_data
    pp_foundation_crypto
    pp_amp_l1
    pp_amp_link
    pp_foundation_identity
    pp_foundation_runtime_core
    pp_common)

if(WIN32)
  target_link_libraries(pp_domain_mesh PUBLIC ws2_32 iphlpapi)
endif()
if(TARGET miniupnpc-static)
  target_link_libraries(pp_domain_mesh PRIVATE miniupnpc-static)
  target_compile_definitions(pp_domain_mesh PUBLIC PP_BROWSER_HAS_MINIUPNPC)
  target_include_directories(pp_domain_mesh PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/miniupnpc/include
    ${CMAKE_BINARY_DIR}/third_party/miniupnpc)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
