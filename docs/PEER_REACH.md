# Peer reach: what we know about reaching peers, and choosing seeds

Status: design agreed 2026-10-21; build order § 7 — steps 1–4 implemented in pp-cpp-mesh (local
branches; amp v2.25.0 and common v1.2.1 released); step 5 partly (pp-browser, local); 6 open.

A mesh node reaches other peers for many jobs — a call's peer, a media hop, a rendezvous relay, a
punch introducer, a directory provider, a DHT peer. Today each job builds its own candidate list,
and none of them remembers whether a peer answered. This document defines one per-peer reach record
the mesh keeps, the roles a peer can play, how the jobs that need "a few live seeds" choose them,
and where the DHT fits. Sections 1–2 describe today; 3–6 the design; 7 the order to build it.

Terms: **seed** = any peer used for a supporting role (relay, introducer, directory, DHT), whether
configured, an org node or a friend's node listed by the directory ("volunteer"). **Peer** = any
mesh PeerId.

## 1. Today

### 1.1 Peer lists

Each list is built separately, with its own sources, order and (no) cap. File references: **M** =
this repo, **B** = pp-browser `src/`.

| List | Built at | Sources, order | Cap / cadence | Used by |
|------|----------|----------------|---------------|---------|
| Effective bootstrap peers | B `foundation/data/MeshRole.cpp:42-88` | directory nodes, then config `bootstrap_peers` (else 2 built-in seeds) | none; per call | DHT endpoints, rendezvous, `bootstrap_seeds` |
| `MeshHopPolicy::bootstrap_seeds` | B `feature/conversations/MeshConnectivityWiring.cpp:68-75` | effective list, or config only (`trusted_relays_only`); org seeds dropped when the probe failed | pushed every 5 s and on directory update | seed warm / park (M `src/reach/CircuitRendezvousCoordinator.cpp`) |
| `MeshHopPolicy::rendezvous_candidates` | B `domain/people/MeshHopPolicy.cpp:169-209` | contacts ∪ directory ∪ DHT ∪ seeds, kept only when a circuit relay (org seed, or listed with `circuit_relay`) | none | reserve, park, `DialableRelayIds` |
| Park surface | M `CircuitRendezvousCoordinator.cpp:248-280` | sticky R1, then Connected, then the rest | park cold-dials the selected ones, staggered 750 ms | PeerReach park gate, Accept |
| Circuit reach relays | M `src/reach/AmpCircuitHopReach.cpp` (`PutAdvertisedRelaysFirst`, `OrderCircuitRelayAttempts`, `CircuitRelayRefusals`) | the relays the target advertised (`PeerReachRequest::advertised_relays`: a call's CallAccept names where the callee is parked), in its order; then sticky R1, Connected, the rest; a relay that refused a circuit in the last 10 min goes behind all but the advertised | 4 bridges in a 14 s envelope, two at once; a `not registered` miss retries once only on an advertised relay | `EnsureViaCircuit` |
| `MeshHopPolicy::punch_introducers` | B `MeshConnectivityWiring.cpp:76-82` | contacts (Connected first), then config seeds | walk with a tried set | cold punch (M `src/reach/PunchIntroducerWalk.cpp`) |
| Directory query peers | B `feature/conversations/ConversationsHub.cpp:916-941` | Amp directory providers, then seeds | none; every 300 s | directory refresh |
| DHT query peers | B `feature/node/NodeMeshServices.cpp:50-68` | directory nodes, then seeds | none | DHT store / find_peer |
| Reachability probe seed | M `src/reachability/ReachabilityEngine.cpp:48-55` | first ADP addr of config `bootstrap_peers` | Node at start; any role on network change | sets `seed_dial_ok` |
| Media hops | B `feature/calls/CallHopRanking.cpp:34-71` | the same merge, ranked by capacity × affinity, filtered by media_relay ads | per call | calls, SoftMigrate |

### 1.2 What is remembered

- Amp's per-peer dial backoff (1 s → 30 s); the seed park clears it before redialing.
- `seed_dial_ok` from the reachability probe — a Client never probes, so it always reads "ok".
- The sticky last-good relay (M `src/reach/AmpCircuitHopReach.cpp:487,532`).
- DHT soft reputation (M `src/dht/client/DhtClient.cpp:72-101`), in memory, DHT only.
- The directory cache's refresh backoff; a call's UI-only "seed unreachable" flag.
- `MeshHopCandidate::recently_failed` and `residual_capacity` exist (M
  `include/mesh/types/MeshHopTypes.h:31-34`) but nothing in production sets them.

A reach result (direct / punched / relayed, or the error) is used once and dropped.

### 1.3 Seed contact over a session

- **All roles:** the directory refresh dials every directory provider and seed every 300 s
  (60 s after a failure) — the only regular seed contact. Its outcomes are not kept per peer.
- **Node role:** the reachability probe at start; the DHT publishes to its query peers every
  max(30 s, ttl/2).
- **During a call:** reserve on every Connected surface peer, renewed every 10 s (a 15 s lease),
  which keeps those links Hot (10 s keepalive, ~25 s liveness window).
- **Between calls:** nothing keeps seed links up; they fall back to the 5 s liveness window and
  are evicted when idle.

### 1.4 What this costs

- A silent dead seed costs **every** call ~10–12 s: the park dials it (8 s Amp dial timeout),
  redials once, then gives up — and the caller's private dial waits behind that park. Seen as the
  pp-browser app lab's `seed_down` flake (5 of 12 runs over 8 s). "Fail early" (b3d10fe) only
  covers seeds that refuse.
- A seed that dies **during** use still looks Connected until its liveness window ends: the park
  answers "parked" at once and the call goes for a circuit through it, losing the circuit budget
  (~16 s).
- The surface is unbounded and the park cold-dials all of it serially; the park's "all Connected"
  rule counts every volunteer.
- The same "is this peer up" question is answered (or not) separately by each list.

## 2. DHT today

- One record type, `PeerRoutingRecord` (signed self-record: multiaddrs, optional caps), keyed by
  raw PeerId; `store` accepts only the sender's own record.
- `find_peer` answers from the local store only; `closer_peers` is always empty
  (M `src/dht/serve/DhtServer.cpp:139`). Lookup is one round to every query peer, no iterative
  walk; `k_bucket_size` is never read. There is **no routing table**: the DHT never learns peers
  beyond its query set (directory nodes + seeds).
- Participation (publish, find_peer from the product) is Node role with `capabilities.dht`
  (default off). Clients only read their (empty) store.
- Consumers: hop lists (`DhtDiscovered` affinity), peer prefetch for chat / calls. Media ranking
  drops DHT hops (`CandidateRelayScopes` returns no scope for them, B
  `domain/people/MeshHopPolicy.cpp:57-63`).
- Not implemented from the contract (B `docs/contracts/MESH_DHT.md`): XOR metric / record keys,
  α/k lookup, `closer_peers`, the publish gate (reachability ≠ Blocked), the negative cache.

In effect the DHT today is "ask the directory nodes and seeds whether they hold a peer's signed
self-record" — a second directory, Node-only.

## 3. Roles, sources and the split

### 3.1 Roles

A peer can be used for any of:

| Role | Needed for | How many live are enough |
|------|------------|--------------------------|
| **Rendezvous** | park / reserve, circuit R1, ServeDial | 2 |
| **Introducer** | coordinated punch | 2 |
| **Directory** | `list_mesh_nodes` | 1 (all providers are asked today) |
| **DHT** | store / find_peer | the DHT's own k (§6) |
| **Media hop** | a group call's media_relay | ranked per call; not a seed job |
| **Probe** | the reachability dial-back | 1 |

A seed usually holds several roles; a contact may be a rendezvous or introducer.

### 3.2 Sources and trust (the app's)

Candidates come from configured seeds, org seeds, friends' nodes listed by the directory
(volunteers — they **count**, as configured seeds do), DHT records and contacts. Which sources are
allowed (`trusted_relays_only`, prefer contacts), how they are ordered and which roles each source
may play is **candidate policy: the app's** (THREADING.md rule 1). The app pushes it.

### 3.3 The split

- **App — candidate policy:** one candidate list with each peer's sources and allowed roles,
  pushed as today (`SetHopPolicy`).
- **Mesh — the reach record (§4):** facts the mesh observes on its I/O lane about each peer.
- **Mesh — role selection (§5):** picks, for a job, up to N candidates from the policy using the
  record. Bounded, deterministic, no app call.
- **Mesh → app — a published record view:** the record as a snapshot (like `MeshLocalView`), so
  app-side ranking (media hops: `recently_failed`) reads the same facts.

## 4. The peer reach record

One record per PeerId, owned by `MeshConnectivity`, living on the mesh I/O lane, in `src/reach/`
(next to `PeerReachCoordinator`). `src/reachability/` stays "is **this** node reachable".

### 4.1 What it holds

| Field | Meaning |
|-------|---------|
| `state` | **Unknown** (no outcome yet), **Live**, **Down** |
| `last_ok_at`, `last_ok_path` | when it was last Connected, and how (direct / punched / relayed) |
| `last_fail_at`, `last_fail_kind` | dial timeout, refused / link table full, keepalive lost, OS unreachable |
| `down_since`, `retry_at` | when it went Down and when it may be tried again |
| `fails_in_row` | for the retry backoff |

Facts only: no addresses (the dial book has them), nothing a peer reports about itself or others.
Bounded (LRU, ~512 peers); in memory — a restart starts Unknown (a Down mark from a previous run
says little about the network now).

### 4.2 What feeds it

Implemented: Amp's link events, subscribed by `MeshConnectivity::Wire` (one listener per Amp
stack) — `Connected` (Live, with its path), `DialFailed` (a dial that missed every dial-book
candidate; pp-cpp-amp adds it for this) and a connected link's `Dropped` that means the peer is
gone: `ConnectionDead` on a link with a keepalive cadence (pp-cpp-amp adds `keepalive_interval_ms`;
a cold link timing out is idle, not dead) or `TransportFailed` (no route). Every outcome below
reaches it through those events — no extra traffic:

- dial outcomes (`EnsureAssociation`), circuit and punch results, reserve answers;
- link events from Amp: Connected, evicted (`ConnectionDead` = keepalive lost, OS unreachable);
- the directory refresh's RPC outcomes (every 300 s) — the regular seed signal;
- the reachability probe.

Transitions: any Connected → **Live**. A failure → **Down** only when it says the peer cannot be
reached now (dial timeout, OS unreachable, keepalive lost on a link we used); "refused / table
full" → Down with a short retry (the peer is up but busy). A network change marks every Down peer
**Unknown** (the old verdict was about the old network).

### 4.3 Retry

A Down peer is not dialed for a job before `retry_at`: 5 min, doubling to 30 min. Outcomes that
arrive anyway (the directory refresh dials it, the peer dials us) update it at once.

## 5. Choosing seeds for a job

`SelectForRole(role, n)` over the pushed candidates allowed that role:

1. **Live** first (most recent `last_ok_at` first, keeping the policy's order among equals),
   then **Unknown** (policy order); **Down** before `retry_at` never.
2. Stop at the role's target (§3.1).

Rules built on it:

- **At most 3 cold dials per round** for a role, stopping once **2 are live**.
- **No dedicated periodic check.** The directory refresh supplies the regular signal. One round
  runs after mesh start and after a network change **only if** no outcome is recent; none while
  the app is backgrounded (mobile).
- **The park:** reserve on the selected set (every Connected relay, then the cold walk, seeds
  first; a cold relay that has not answered in 750 ms does not hold up the next — Happy Eyeballs:
  dialed one after another, an unreachable one cost its whole 8 s dial timeout, and a call's caller
  had walked its relays before the answerer was parked anywhere, pp-browser #502), and wait on
  **bootstrap seeds only**: settle once enough are Connected — the target, or
  every seed not held down (and dialable) when there are fewer; ≥1 after a short grace or at the
  deadline, as before. When every seed is held down, the park answers **not parked at once**.
  A seed's miss is redialed within the park only when it may pass (the record does not hold it
  down, or it refused — up but busy). Other rendezvous relays are never waited on or redialed: one
  may be the very peer being reached (a contact that relays), and dialing it is the dual-NAT dial
  the park exists to prevent (hard lab `B-HARD-CALL-NAT-COLD-DIRTY` caught a surface-wide park).
  The park runs on a bench too: `mesh::test::ParkHarness` (`mesh/testing/park_harness.h`) puts a
  phone's MeshHost and the product's `CircuitRendezvousCoordinator` with relays that answer, answer
  slowly (`DelayedDatagramIo`: every exchange `rtt_ms` longer) or never do, on one in-memory hub and
  one virtual clock; `src/reach/tests/park_test.cpp` checks the stagger (an unreachable first seed,
  parked in under 2 s — 8 s serially; a near seed behind a 1.5 s one, under 2 s — 4.6 s serially),
  that slow seeds are still parked on, the walk's stop at two live relays, seeds first, the chosen
  R1 and the deadline. A park rule changes there first; the app lab
  (pp-browser `call_path_*_two_hops`) is the end-to-end check.
- **Circuit dial:** the relays circuit reach may bridge through leave out those held down
  (unless Connected) — a StartBridge through one would only time out. Relays the target
  advertised go first (pp-browser media-hop-reachability H013): the target knows where it holds a
  reservation; the dialer only guesses. Only those get a second try on `not registered`.
  The walk bridges to the first two relays at once (`kCircuitParallelBridges`) and keeps two out
  as it goes down the list: one that never answers no longer costs its whole bridge timeout (4 s)
  before the next is tried (pp-browser #577: 9.6 s for a circuit whose first relay was silent,
  ~3.2 s otherwise). The first bridge to ack wins and the nested link is established over it
  alone; a second ack meanwhile is released, and once the nested link is up every bridge still
  out is cancelled. A dropped bridge that had acked has its channel to the relay closed
  (`CancelTunnel` only forgets a bridged tunnel), so its relay ends the splice — nothing nested
  rides it. Both missing, the walk goes on down the list; the 4-bridge cap, the 14 s envelope,
  the order and the refusal memory are as before. Costs: for a moment two relays each set up a
  bridge for one reach, and the target sees a second carrier that closes at once. On a bench:
  `src/reach/tests/circuit_walk_test.cpp` (`ParkHarness::AddCaller`: a caller whose circuit
  reach bridges to the parked phone, virtual ms) — a silent first relay, then a live one: under
  2 s (4.6 s one at a time); two live: one splice left, at the winner.
- **Awaiting side:** `PeerReachRequest::await_hold_ms` keeps an await open at least that long from
  its start, waiting for the peer's circuit (a call's answerer: the caller starts only once our
  CallAccept crossed its inbox). The waiter ends it sooner by cancelling.
- **PeerReach:** the park before a private dial uses the selection; with every rendezvous peer
  Down it answers at once and the private dial goes ahead. A same-LAN private Preferred (on one
  of our listen addresses' /24s, `PeerReachPorts::local_listen_addrs`) is dialed first, without
  the park — the dual-NAT case the park guards (fd4e3de) cannot arise there. Two homes on the
  same 192.168.1.0/24 look alike, so that dial gets 1.5 s (a LAN handshake takes milliseconds);
  without an answer it is abandoned and the reach parks as before. Reach only: the awaiting side
  keeps its seed link untouched.
- **Racing the circuit** (`PeerReachRequest::race_circuit`, off by default): by default a reach
  gives its direct dial a 12 s budget (redialing within it) and only then builds the circuit — a
  peer the dial cannot reach costs 12 s and then the circuit (pp-browser #577: a 9 s handshake
  timeout, the pivot at 12 s, then a 9.6 s circuit). With the option the circuit starts with the
  dial (after its seed park, as always) and the first link to connect settles the reach: a direct
  link that lands first still settles it Direct; a circuit that lands first, Relayed. Neither is
  cancelled when the other wins — the circuit is shared (`AbortPending` would end every reach's),
  and the dial's state is Amp's (`FinishDial`): a late relayed link stays as a standby, a late
  direct link is Amp's to prefer. The racing circuit does not punch (`TryEnsureCircuitOnlyAsync`);
  the dial keeps redialing within its budget, and the punch — the last resort — runs only once the
  dial is done or past its budget, so it never shares the peer's path with our own dial. Only a
  dial to a public Preferred is raced: a private one is still parked for first, and once parked it
  is skipped and the circuit starts at once anyway (H010). Await and `exclude_direct` ignore it.
  The reach deadline covers the circuit from its start (16 s), the dial overlap after a miss
  (V049) and the watchdog as before. For: call pairs whose relay is the anchor anyway — any mobile
  end (pp-browser K013), where a direct path would not be kept; pp-browser sets it there. Costs a
  circuit (a bridge at a relay) for a pair whose direct dial would have landed. On a bench:
  `src/reach/tests/peer_reach_race_test.cpp` (one ManualExecutor lane, virtual ms;
  `PeerReachPorts::now_ms` is the lane's clock).
- **Introducers:** the punch walk skips an introducer the record holds down (unless its link is
  up) and dials at most kReachMaxColdDialsPerRound introducers that are not connected, per walk.
- **Directory / DHT query peers:** the app lists them, so the mesh offers the selection:
  `MeshConnectivity::ChoosePeers(candidates, max)` — up peers first, then the record's latest
  successes, then the rest in the app's order, none held down. The directory client takes it as
  `AmpDirectoryProtocolConfig::choose_query_peers` (a list asks only the chosen peers; none
  worth asking answers at once, and the app falls back to HTTP). DHT query peers wait for the
  DHT design (§ 6) — the DHT client keeps its own reputation today. `ReachFacts(peer_id)` is the
  published view (media ranking's `recently_failed`).
- **Probe:** left as it is. It runs at mesh start (the record is empty) and right after a network
  change (every Down mark reads Unknown then) — the record has nothing to say at either moment.

## 5a. Service load

A node says how busy it is, per service, as it judges it: not the numbers behind it (`ServiceLoad.h`; pp-browser
p2p-mesh N033). The reach record (§4) holds what the mesh observes; what a node says of itself is kept apart, in the
host's `PeerLoadBook`.

- **Services:** `park` (a circuit reservation and the link holding it), `relay` (a 1:1 call's bridge),
  `media_relay` (hosting a group call).
- **Levels**, each an action for the client:

  | Level | The client |
  |-------|------------|
  | `open` | prefers it |
  | `busy` | uses it when nothing better is around |
  | `full` | sends it nothing new; it would be refused (`code: "busy"`). What it already serves goes on: a phone parked there renews, a call it hosts still takes its members |
  | `draining` | moves off when it can; it takes nothing new and is going away |

- **On the wire:** `"load": "open" | "busy" | "full" | "draining"` beside `ok`, in circuit reserve and bridge replies
  (the park level for a reserve, the relay level for a bridge) and media_relay quote / accept replies — refusals
  included. Absent: an older node, or nothing to say (Unknown). An unknown word reads Unknown. Beside it,
  `"loads": {"park": …, "relay": …, "media_relay": …}` says every service the node serves (v6.4.0), so a phone that
  parks on a hop knows its group-call level before planning a call there (`NoteReplyLoads`); a service it does not
  serve is not named. Each server also republishes its pressures on its own tick, not only on a request: a level
  others read from other services' replies must not stay full after the work ended.
- **The node's rule** (`ServiceLoadMonitor`): each service's pressure is the worst of its sources, 0 (idle) to 1 (at
  capacity) — reservations, bridges, hosted calls, the link table (sampled once a second), and what the operator feeds
  in (CPU, uplink: `SetPressure`). Busy from 0.70 (back to open below 0.60), full from 0.95 (back below 0.90): a gap
  between on and off, so a node near a line does not flap. Draining (the operator's) sets every service. The servers
  admit by the same level they say: full is exactly when a node refuses new work. The rule can change without a
  client: clients act on the level only. The levels are on the node's metrics as `pp_service_load{service}` and
  `pp_service_pressure{service}`.
- **The client:** a level is good for 2 min (`PeerLoadBook::kFreshMs`), then reads Unknown.
  - The park (`CircuitRendezvousCoordinator`) does not count a relay that said draining, nor one that said full
    unless it holds a reservation there (its renewal is accepted). It skips them for the round, orders the rest open
    before busy (seeds still first), and on a full refusal or a draining ack starts a new round at once (at most every
    5 s) instead of at the app's next renewal. A reservation held on a draining relay is renewed meanwhile.
  - The bridge walk (`AmpCircuitHopReach`) tries open relays before busy ones and full / draining ones last; the
    relays the target advertised still go first (H013).
  - The app's group-call hop ranking reads `PeerLoads()` for `media_relay`.

## 6. Where the DHT fits

- **Now:** DHT RPCs are dials, so their outcomes feed the record. Its records stay a candidate
  **source** (§3.2) — signed self-records of reachable nodes. DHT hops are scoped **Public**
  (strangers): media ranking offers them only to consumers that take Public hops.
- **Later:** completing it — keys and XOR distance, a routing table, `closer_peers`, iterative
  lookup, publishing at the closest nodes, the publish gate, the negative cache — is a design of
  its own: docs/DHT.md (agreed). Until it is built the DHT is a second directory and must not
  hold up calls.

## 7. Build order

Status: 1–6 done. Step 5's last part (pp-cpp-mesh after v1.3.0): the app pushes one candidate
list, each candidate with its roles (`MeshHopCandidate::roles`, `MeshReachRole`); the mesh reads
the rendezvous surface, the bootstrap seeds and the introducers from it (`MeshHopPolicy::
Rendezvous` / `Seeds` / `Introducers`). The DHT (step 6) is docs/DHT.md.

Each step ships on its own (unit tests + the pp-browser labs):

1. **Settle on connect during a park** — done on branch `fix/seed-park-settles-on-connect`.
2. **The record** (§4), fed by dial / link / circuit / punch / directory outcomes; published view.
3. **Seed selection for rendezvous** (§5): bounded park and reserve, "all Down → answer now",
   the same-LAN private dial. Fixes `seed_down`; add a lab `seed_dies_in_use` (seeds used, then
   stopped; call inside and after the liveness window).
4. **Introducers** use the record; **directory and DHT query peers** get `ChoosePeers` for the app
   to call; the probe stays (§ 5).
5. **The app's lists** (pp-browser): the directory config sets `choose_query_peers` to
   `ChoosePeers` (`ConversationsHub::ConfigureAmpDirectoryProtocol`, `NodeMeshServices`); media ranking (`CallHopRanking`) reads
   `recently_failed` from `ReachFacts`; DHT hops get a relay scope; then one candidate list with
   roles in `MeshHopPolicy` — `bootstrap_seeds` / `punch_introducers` / effective-bootstrap
   merging go.
6. **DHT completion** — separate design (§6).

## 8. Open

- **A seed that dies while its link still looks Connected** (§1.4): addressed. A park asks every
  Connected seed not heard from in the last 5 s to prove it is alive (Amp `VerifyLink`: a probe;
  no answer within ~2 s evicts the link as `unresponsive`, the record marks the seed down) and
  counts it only once it answers. A live seed costs the park one round trip; a dead one ~2 s
  instead of a circuit through it. Same-LAN pairs dial before the park anyway (§ 5).
- **Persisting Down marks** across restarts: not now (§4.1); revisit if cold starts stay slow.
- **Targets per role** (§3.1) are first guesses; the labs will tell.
- **One peer, two dial keys** (addressed): the reachability probe dialled a seed as
  `reachability:seed` while the DHT / directory dial it by PeerId; dialled at once, the two links
  dropped each other (`connection-closed`) and the first exchanges on the other key failed (seen
  in pp-browser's `pp_node_dht_smoke.sh`). The probe now dials the seed under its PeerId (and
  keeps an endpoint already registered for it). The DHT also retries a publish nobody answered in
  10 s and looks query peers up again every 15 s. The seed's dial-back keeps its own key on
  purpose: it must prove a fresh inbound dial.

## Related

- THREADING.md (here) — lanes, and rule 1 (candidate policy is the app's).
- pp-browser `docs/contracts/MESH_DHT.md`, `docs/contracts/MESH_DIRECTORY_AMP.md`,
  `docs/ops/CONFIGURATION.md` (Mesh DHT, Trusted relays only), `docs/architecture/MESH.md`.
- pp-browser `projects/media-hop-reachability/DECISIONS.md` H010 (seed park gate), H011 (R1
  rendezvous); `projects/p2p-mesh/DECISIONS.md` N015, N028; `projects/p2p-mesh/DESIGN.md`
  (bootstrap seed, DHT, relay selection).
