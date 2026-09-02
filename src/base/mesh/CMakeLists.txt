# Product Amp mesh glue (base layer). Libp2p Host/Yamux/Noise L4 retired from product (A017/D10).

set(PP_BASE_MESH_IDENTITY_SOURCES
  identity/MlDsaPublicKeyWire.cpp
  identity/Multihash.cpp
  identity/Base58.cpp
  identity/PeerId.cpp
  identity/PeerIdUtil.cpp
)

pp_browser_add_base_library(pp_base_mesh_identity
  SOURCES
    ${PP_BASE_MESH_IDENTITY_SOURCES}
  PUBLIC_LIBS
    pp_base_crypto
    pp_common)

if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp_base_mesh_identity PUBLIC OpenSSL::Crypto)
endif()

set(PP_BASE_MESH_HOST_SOURCES
  host/MeshHost.cpp
  host/MeshPorts.cpp
)

set(PP_BASE_MESH_REACHABILITY_SOURCES
  reachability/Reachability.cpp
  reachability/ReachabilityService.cpp
  reachability/NatTraversal.cpp
  reachability/LanMdnsDiscovery.cpp
  reachability/AmpDialBackService.cpp
)

set(PP_BASE_MESH_DISCOVERY_SOURCES
  discovery/MeshDirectoryCache.cpp
)

set(PP_BASE_MESH_DHT_SOURCES
  dht/DhtRecordCodec.cpp
  dht/DhtRecordStore.cpp
  dht/DhtRateLimiter.cpp
  dht/AmpDhtService.cpp
)

set(PP_BASE_MESH_L4_CIRCUIT_SOURCES
  l4/circuit/AmpCircuitHopRegistry.cpp
  l4/circuit/CircuitBundleLogic.cpp
  l4/circuit/CircuitTunnelCoordinator.cpp
)

set(PP_BASE_MESH_L4_MEDIA_RELAY_SOURCES
  l4/media_relay/MediaRelayFrames.cpp
  l4/media_relay/MediaRelayAttachSm.cpp
  l4/media_relay/MediaRelayLogic.cpp
  l4/media_relay/MediaRelayBundleLogic.cpp
  l4/media_relay/AmpMediaRelayCoordinator.cpp
)

set(PP_BASE_MESH_L4_CALL_MEDIA_SOURCES
  l4/call_media/CallMediaLegCoordinator.cpp
  l4/call_media/CallMediaAmpTransport.cpp
  l4/call_media/CallMediaBundleLogic.cpp
  l4/call_media/CallMediaSessionLogic.cpp
  l4/call_media/CallMediaFrameCrypto.cpp
)

if(WIN32)
  list(APPEND PP_BASE_MESH_REACHABILITY_SOURCES
    reachability/LanMdnsSocket_Win32.cpp
    reachability/ReachabilityNetIf_Win32.cpp)
else()
  list(APPEND PP_BASE_MESH_REACHABILITY_SOURCES
    reachability/LanMdnsSocket_Posix.cpp
    reachability/ReachabilityNetIf_Posix.cpp)
endif()

pp_browser_add_base_library(pp_base_mesh
  SOURCES
    ${PP_BASE_MESH_HOST_SOURCES}
    ${PP_BASE_MESH_REACHABILITY_SOURCES}
    ${PP_BASE_MESH_DISCOVERY_SOURCES}
    ${PP_BASE_MESH_DHT_SOURCES}
    ${PP_BASE_MESH_L4_CIRCUIT_SOURCES}
    ${PP_BASE_MESH_L4_MEDIA_RELAY_SOURCES}
    ${PP_BASE_MESH_L4_CALL_MEDIA_SOURCES}
  PUBLIC_LIBS
    pp_base_data
    pp_base_crypto
    pp_base_adp
    pp_base_mesh_link
    pp_base_mesh_identity
    pp_base_runtime_core
    pp_common)

if(WIN32)
  target_link_libraries(pp_base_mesh PUBLIC ws2_32 iphlpapi)
endif()
if(TARGET miniupnpc-static)
  target_link_libraries(pp_base_mesh PRIVATE miniupnpc-static)
  target_compile_definitions(pp_base_mesh PUBLIC PP_BROWSER_HAS_MINIUPNPC)
  target_include_directories(pp_base_mesh PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/miniupnpc/include
    ${CMAKE_BINARY_DIR}/third_party/miniupnpc)
endif()
target_link_libraries(pp_base_mesh PRIVATE pp_base_net)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
