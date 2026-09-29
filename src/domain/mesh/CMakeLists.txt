# Product Amp mesh glue (domain peer). Libp2p Host/Yamux/Noise L4 retired from product (A017/D10).
# PeerId / identity lives under src/foundation/identity/ (pp_foundation_identity).
#
# One static library per folder; the link edges below are the allowed include edges
# (scripts/check/check_mesh_layers.sh keeps the two in step). Bottom to top:
#
#   shared (shared/ + l4/shared/)
#   reachability   dht <- discovery   circuit <- media_relay   call_media
#   host           (MeshHost: composes the services above)
#   reach          (reach / rendezvous / punch walk over MeshHost)
#   media_plane    (MeshMediaPlane, MediaRelayAttach)
#
# `pp_domain_mesh` aggregates them for consumers outside the peer.

set(_pp_mesh_base_libs
  pp_foundation_data
  pp_foundation_crypto
  pp_amp_l1
  pp_amp_link
  pp_foundation_identity
  pp_foundation_runtime_core
  pp_common)

# pp_domain_mesh_<name>: sources are relative to this directory.
function(_pp_mesh_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  set(_deps "")
  foreach(dep IN LISTS ARG_DEPS)
    list(APPEND _deps pp_domain_mesh_${dep})
  endforeach()
  pp_browser_add_domain_library(pp_domain_mesh_${name}
    SOURCES ${ARG_SOURCES}
    PUBLIC_LIBS ${_pp_mesh_base_libs} ${_deps})
endfunction()

_pp_mesh_library(shared
  SOURCES
    l4/shared/MediaFrameBody.cpp)

set(_pp_mesh_reachability_sources
  reachability/Reachability.cpp
  reachability/ReachabilityEngine.cpp
  reachability/NatTraversal.cpp
  reachability/LanMdnsDiscovery.cpp
  reachability/AmpDialBackProtocol.cpp
  reachability/AmpObservedAddrs.cpp
  reachability/PunchLogic.cpp
  reachability/PunchBurst.cpp
  reachability/AmpPunchCoordinator.cpp
  reachability/MobileEphemeralListenGate.cpp)
if(WIN32)
  list(APPEND _pp_mesh_reachability_sources
    reachability/LanMdnsSocket_Win32.cpp
    reachability/ReachabilityNetIf_Win32.cpp)
else()
  list(APPEND _pp_mesh_reachability_sources
    reachability/LanMdnsSocket_Posix.cpp
    reachability/ReachabilityNetIf_Posix.cpp)
endif()
_pp_mesh_library(reachability
  SOURCES ${_pp_mesh_reachability_sources}
  DEPS shared)
if(WIN32)
  target_link_libraries(pp_domain_mesh_reachability PUBLIC ws2_32 iphlpapi)
endif()
if(TARGET miniupnpc-static)
  target_link_libraries(pp_domain_mesh_reachability PRIVATE miniupnpc-static)
  target_compile_definitions(pp_domain_mesh_reachability PUBLIC PP_BROWSER_HAS_MINIUPNPC)
  target_include_directories(pp_domain_mesh_reachability PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/miniupnpc/include
    ${CMAKE_BINARY_DIR}/third_party/miniupnpc)
endif()

_pp_mesh_library(dht
  SOURCES
    dht/DhtRecordCodec.cpp
    dht/DhtRecordStore.cpp
    dht/DhtRateLimiter.cpp
    dht/AmpDhtProtocol.cpp
  DEPS shared)

_pp_mesh_library(discovery
  SOURCES
    discovery/MeshDirectoryCache.cpp
    discovery/NameDirectory.cpp
    discovery/MeshNodeHitCodec.cpp
    discovery/AmpDirectoryProtocol.cpp
  DEPS dht shared)

# circuit: shared wire / policy at the root; serve/ = the relay side, client/ = bridges and reserves.
_pp_mesh_library(circuit
  SOURCES
    l4/circuit/CircuitBundleLogic.cpp
    l4/circuit/serve/CircuitRelayServer.cpp
    l4/circuit/client/CircuitClientCoordinator.cpp
    l4/circuit/client/AmpCircuitHopRegistry.cpp
  DEPS shared)

# media_relay: shared wire / decisions at the root; serve/ = the hop side, client/ = the attaching side.
_pp_mesh_library(media_relay
  SOURCES
    l4/media_relay/MediaRelayFrames.cpp
    l4/media_relay/MediaRelayLogic.cpp
    l4/media_relay/MediaRelayBundleLogic.cpp
    l4/media_relay/serve/MediaRelayAttachSm.cpp
    l4/media_relay/serve/MediaRelayServer.cpp
    l4/media_relay/client/MediaRelayClientCoordinator.cpp
    l4/media_relay/client/AmpMediaRelayClient.cpp
    l4/media_relay/client/MediaRelayFrameCrypto.cpp
  DEPS circuit shared)

_pp_mesh_library(call_media
  SOURCES
    l4/call_media/CallMediaLegCoordinator.cpp
    l4/call_media/CallMediaAmpTransport.cpp
    l4/call_media/CallMediaBundleLogic.cpp
    l4/call_media/CallMediaSessionLogic.cpp
    l4/call_media/CallMediaFrameCrypto.cpp
  DEPS shared)

_pp_mesh_library(host
  SOURCES
    host/AmpLinkConfig.cpp
    host/LocalNetworkChange.cpp
    host/MeshHost.cpp
    host/MeshLinkEventLog.cpp
    host/MeshPorts.cpp
    host/MeshPumpThread.cpp
  DEPS reachability dht discovery circuit media_relay call_media shared)

_pp_mesh_library(reach
  SOURCES
    reach/AmpCircuitHopReach.cpp
    reach/PeerReachCoordinator.cpp
    reach/PunchIntroducerWalk.cpp
    reach/CircuitRendezvousCoordinator.cpp
  DEPS host reachability circuit shared)

_pp_mesh_library(media_plane
  SOURCES
    media_plane/MeshMediaPlane.cpp
    media_plane/MediaRelayAttach.cpp
  DEPS reach host reachability media_relay)

add_library(pp_domain_mesh INTERFACE)
target_link_libraries(pp_domain_mesh INTERFACE
  pp_domain_mesh_media_plane
  pp_domain_mesh_reach
  pp_domain_mesh_host
  pp_domain_mesh_call_media
  pp_domain_mesh_media_relay
  pp_domain_mesh_circuit
  pp_domain_mesh_discovery
  pp_domain_mesh_dht
  pp_domain_mesh_reachability
  pp_domain_mesh_shared)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
