# Media hop reachability — current state

> Moved from pp-browser `projects/media-hop-reachability/` at [44bf5c0](https://github.com/people-post/pp-browser/tree/44bf5c0221db8bdd3076b20ab460939c045fda32/projects/media-hop-reachability) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)); its decision ids (H001–H013) are unchanged. Links back into pp-browser point at its `main`.

> **2026-09:** Product mesh is Amp-only; hop reachability uses `AmpCircuitHopReach` + `MeshHost::CircuitDeps()`. Hole punch planned as **Amp Coordinated Punch** ([HOLE_PUNCH.md](HOLE_PUNCH.md), H009) — not libp2p DCUtR. See [MESH.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/MESH.md).

**Last updated:** 2026-10-31 (H013: the callee advertises its parked relays; the answerer waits for the caller — issue #416)

## Direction

Hop **reachability** = **Amp mesh stack work** (H001/H007). App-layer `call_hop_addrs` prototype **removed**; do not re-land without ADR. Preference: **publish → punch → circuit → fail** (H002).

**Spec:** [DESIGN.md](DESIGN.md) (concept-first). **Build order:** [PHASES.md](PHASES.md).

`CallStack` builds `AmpCircuitHopReach` whenever Amp circuit tunnel is started — **not** gated on `capabilities.circuit_relay` (that flag is Node hosting only).

## Landed

| Area | State |
|------|-------|
| Project docs | Ownership: Amp mesh implements; SoftMigrate consumes |
| ADRs | H001–H013; circuit multi-hop plan [N024](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/DECISIONS.md#n024--immediate-relay-as-service-broker); punch plan H009; **H010** StartBridge budget; **H011** R1 rendezvous (spec) |
| **L1 peer address book** | Stack upsert on bootstrap/register/connect/dial-success; preferred dial addr helpers |
| **L2 advertised listen set** | Amp ch0 + dial-back / UPnP-derived ads |
| **L3 circuit PeerId dial** | Circuit tunnel / hop reach — **single-hop**; SoftMigrate circuit fallback via `ICircuitHopReach` / `AmpCircuitHopReach`; **H010** attempt budget (envelope, ≤4 StartBridge, sticky+Connected order) |
| **L3 compose (loopback)** | Shared loopback partition fixture; call-media via R; media_relay quote/attach/fan-out via R |
| **Circuit reserve** | `op=reserve` + `StartReserve` — answerer parks **shared rendezvous surface** (`BuildCircuitHopList`, Connected-all + serial cold; H011 L3.1a) |
| **L3.1a R1 rendezvous** | Shared collect/reserve builder + `CircuitRendezvousPolicy.h`; dialer/answerer no longer seeds-only asymmetric |
| **L3.1b–d** | Sticky park + `call_circuit_r1` announce (flush after Invite) + hard-w5 STACK gate |
| **H013** | CallAccept `circuit_parked` (from `MeshLocalView::parked_relays`) tried first by the caller; same-relay not-reg retry only on advertised relays; answerer `await_hold_ms` 60 s, ended early by `call_connect_give_up` (pp-cpp-mesh v2.4.0) |

## In progress / gaps

| Area | State |
|------|-------|
| **L3.1 Circuit R1 rendezvous** | **L3.1a–d landed** — [CIRCUIT_R1_RENDEZVOUS.md](CIRCUIT_R1_RENDEZVOUS.md) |
| **L3.25 Amp Coordinated Punch** | Spec done; **L3.25a–c complete** + gap tests — dual-dial A026 race, sync-window expiry → PunchFailed / SoftMigrate circuit fallback; hard-lab must not overclaim CGNAT punch — [HOLE_PUNCH.md](HOLE_PUNCH.md) |
| **L3.5 multi-hop circuit** | Spec done — [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md); parallel to punch |
| **L4 SoftMigrate consume** | Rank hops; skip undialable after circuit; drop empty contact ma — **loopback compose green**; punch upsert flips `IsDialable` (L3.25b) |
| **L5 directory / DHT** | Planned; closed-set for media hops |

## Code anchors (elsewhere)

| Piece | Location |
|-------|----------|
| SoftMigrate hop pick | `MeshHopPolicy`, `CallTopologyController` |
| Custom circuit | `CircuitTunnelCoordinator`, circuit L4 |
| DialBack / Reachability / UPnP | `src/lib/mesh/reachability/` |
| Amp underlay | pp-cpp-amp (`PeerLink`, keepalive, `MaybeLearnPath`) |
| Hop reach helper | `AmpCircuitHopReach` |
| R1 rendezvous policy | `CircuitRendezvousPolicy.h`, `CallMediaPlane::BuildCircuitRendezvousCandidates` |
| Partition compose tests | `src/lib/mesh/tests/` (`amp_circuit_*_compose_test`, loopback fixture) |
| Hard lab (forced A↛B nets) | Design: [HARD_LAB.md](https://github.com/people-post/pp-browser/blob/main/packaging/pp-node/HARD_LAB.md); delivery [hard-lab](https://github.com/people-post/pp-browser/tree/main/projects/hard-lab) — Wave 1–2 scaffold |

## Next

1. **L3.5** — multi-hop circuit v2 when transitive reachability is needed (R1↛B, R2 can) — parallel
2. Mesh invest: [N022](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup)
3. Punch hard-lab Wave 7 (**N-HARD-HOLEPUNCH**) only after measured NAT shapes — do not treat Wave 5 CGNAT-ish as punch coverage
