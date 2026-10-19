# pp-cpp-mesh

The mesh layer on [pp-cpp-amp](https://github.com/people-post/pp-cpp-amp): a mesh host and the
services it composes — connectivity (dial registry, listen book), reach and rendezvous, reachability
(dial-back, hole punching, LAN mDNS, UPnP), a DHT, a directory, and circuit and media relays (both
ends). Namespace `pp::mesh`, includes `mesh/…`.

It moved here from [pp-browser](https://github.com/people-post/pp-browser) (`src/domain/mesh/`) with
its history; pp-browser and its headless node `pp-node` are its first consumers.

## Rules

- **Dependencies:** [pp-cpp-common](https://github.com/people-post/pp-cpp-common),
  [pp-cpp-crypto](https://github.com/people-post/pp-cpp-crypto) and pp-cpp-amp only (plus the
  vendored miniupnpc, off by default). No application code: quoted includes are `mesh/…` or a
  dependency's (`common/…`, `crypto/…`, `amp/…`) — `scripts/check_layers.sh` holds the line.
- **Protocols and bytes, not product types.** Product policy (which peers make good hops, contacts,
  UI) stays with the app and is pushed in as snapshots.
- **Threads:** control runs on the mesh's I/O lane; the host gives each instance `work` / `blocking`
  executors and a metrics registry; the mesh never calls app code synchronously —
  [docs/THREADING.md](docs/THREADING.md).
- **Wire IDs are contracts.** Protocol IDs keep their `/pp-browser/…` names (and the mDNS service type
  its `_pp-browser._tcp`): renaming one breaks every deployed peer.

## Layout

| Folder (`include/mesh/`, `src/`) | Library | What |
|----------------------------------|---------|------|
| `types/` | `pp_mesh_types` | Vocabulary shared with app code: directory rows, hops, relay scope, address disclosure, public address, media channel, config. pp-cpp-common only — an app's lower layers can use it without Amp |
| `shared/`, `l4/shared/` | `pp_mesh_shared` | Executors, protocol IDs, channel policies, frame bodies |
| `reachability/` | `pp_mesh_reachability` | Reachability engine, dial-back, punch, LAN mDNS, UPnP |
| `dht/` | `pp_mesh_dht` | DHT records, store, server and client |
| `discovery/` | `pp_mesh_discovery` | Directory protocol, name directory, directory cache |
| `l4/circuit/` | `pp_mesh_circuit` | Circuit relay (serve / client) |
| `l4/media_relay/` | `pp_mesh_media_relay` | Media relay (serve / client) |
| `host/` | `pp_mesh_host` | `MeshHost`: composes the services above on one Amp stack |
| `reach/` | `pp_mesh_reach` | Peer reach, circuit hop reach, rendezvous, punch walk |
| `connectivity/` | `pp_mesh_connectivity` | `MeshConnectivity`: dial registry, listen book |
| `media_plane/` | `pp_mesh_media_plane` | `MeshMediaRelay`, `MediaRelayAttach` |
| `testing/` | `pp_mesh_testing` | In-memory Amp harnesses, for these tests and consumers' (not in `pp_mesh`) |

`pp_mesh` links them all but `testing`. Each library's `DEPS` in `src/CMakeLists.txt` are its allowed
include edges.

## Build

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DPP_MESH_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
scripts/check_layers.sh
```

| Option | Default | |
|--------|---------|---|
| `PP_MESH_BUILD_TESTS` | `OFF` | Unit and in-memory integration tests (GoogleTest) |
| `PP_MESH_WARNINGS_AS_ERRORS` | `ON` | `-Werror` / `/WX` on this library's targets |
| `PP_MESH_WITH_UPNP` | `OFF` | UPnP port mapping (vendored miniupnpc 2.3.3; never on Android / iOS) |
| `PP_CPP_{COMMON,CRYPTO,AMP}_SOURCE_DIR` | — | A local checkout instead of the pinned release tag |

Dependencies are release tags fetched with FetchContent (`cmake/PpMeshDeps.cmake`); a consumer that
already defines `pp_common`, `pp_crypto` and `pp_amp_link` keeps its own. Consumers fetch this repo the
same way and link `pp_mesh` (and `pp_mesh_testing` in tests).
