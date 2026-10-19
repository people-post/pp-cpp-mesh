# One static library per folder, each defined in its folder's CMakeLists.txt with its unit tests in
# tests/ beside it. A library's DEPS are its allowed include edges (scripts/check_layers.sh keeps
# the two in step). Bottom to top:
#
#   types          the vocabulary apps share: directory, hops, relay scope, disclosure, config —
#                  pp-cpp-common only, so an app's lower layers can use it without Amp
#   shared         shared/ + l4/shared/
#   reachability   dht <- discovery   circuit <- media_relay   (l4/)
#   host           MeshHost: composes the services above
#   reach          reach / rendezvous / punch walk over MeshHost
#   connectivity   MeshConnectivity: dial registry, listen book, circuit reach
#   media_plane    MeshMediaRelay, MediaRelayAttach
#   testing        in-memory Amp harnesses for tests here and consumers' — not in pp_mesh

add_subdirectory(types)
add_subdirectory(l4)
add_subdirectory(reachability)
add_subdirectory(dht)
add_subdirectory(discovery)
add_subdirectory(host)
add_subdirectory(reach)
add_subdirectory(connectivity)
add_subdirectory(media_plane)
add_subdirectory(testing)

# Everything but the test harness.
add_library(pp_mesh INTERFACE)
target_link_libraries(pp_mesh INTERFACE
  pp_mesh_media_plane
  pp_mesh_connectivity
  pp_mesh_reach
  pp_mesh_host
  pp_mesh_media_relay
  pp_mesh_circuit
  pp_mesh_discovery
  pp_mesh_dht
  pp_mesh_reachability
  pp_mesh_shared
  pp_mesh_types)
