# Media hop reachability — decisions

> Moved from pp-browser `projects/media-hop-reachability/` at [44bf5c0](https://github.com/people-post/pp-browser/tree/44bf5c0221db8bdd3076b20ab460939c045fda32/projects/media-hop-reachability) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)); its decision ids (H001–H013) are unchanged. Links back into pp-browser point at its `main`.

Call: [V026](https://github.com/people-post/pp-browser/blob/main/projects/p2p-av-calls/DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking). Mesh: [N022](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup). Amp mesh: [NETWORKING.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/NETWORKING.md), [MESH.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/MESH.md).

---

## H001 — Separate project; implementation in Amp mesh

**Status:** Accepted (updated 2026-09-04 — Amp underlay)  
**Decision:** This project owns the **program and consume contract**. **Implementation of reachability** lives in **Amp + `src/lib/mesh/`** (ch0 addr ads, addr book, DialBack, UPnP, circuit, **Amp Coordinated Punch**). Calls/mesh **consume** dialability; they do not reimplement NAT traversal. In-tree libp2p retains PeerId/crypto helpers only (A017) — not a product Host/DCUtR path.  
**Rationale:** One peer stack (N022/V026/D10); app ICE-alikes diverge and rot.  
**Alternatives:** Forever SoftMigrate-only multiaddr glue (rejected — H007); revive libp2p DCUtR as product punch (rejected — A017).

---

## H002 — Publish in-stack > punch > circuit > fail

**Status:** Accepted (updated 2026-09-04 — insert ACP before circuit)  
**Decision:** Prefer **stack address book + Reachable ads**, then **Amp Coordinated Punch** ([H009](#h009--amp-coordinated-punch-acp)), then **circuit**, then SoftMigrate failure. No product dependency on mid-call addr gather.  
**Rationale:** Direct PeerLinks are cheaper/lower-latency than TURN-analogue circuit when NATs are punchable; circuit remains the PeerId-path fallback.  
**Alternatives:** App `call_hop_addrs` primary (rejected); punch after multi-hop only (rejected — parallel tracks).

---

## H003 — Contacts may mirror stack addrs (cache only)

**Status:** Accepted  
**Decision:** Contact `multiaddrs` remain a **TTL UX/cache** optionally filled from the stack — not the source of truth for dial. PeerId is identity.  
**Rationale:** SoftMigrate already reads contacts; truth should move to host address book.  
**Alternatives:** Contacts-only forever.

---

## H004 — No WebRTC / no app STUN for hops

**Status:** Accepted (updated 2026-09-04 — Amp observed addrs)  
**Decision:** No WebRTC ICE/STUN for hop dial. Observed addrs via **DialBack / ch0 ads / UPnP** (and later punch success) inside Amp mesh.  
**Rationale:** V026; one stack.  
**Alternatives:** libjuice STUN for advertise only (rejected as product path).

---

## H005 — Circuit last resort; bill media hop

**Status:** Accepted (updated 2026-08-01 — brokered path via N024)  
**Decision:** Circuit may enable dial to hop PeerId. Prefer contact then seed bridges (N014). Evolve custom circuit toward PeerId-friendly semantics (L3). Successful **Amp Coordinated Punch** prefers **direct attach** over brokered.

**Billing — two attach modes:**

| Mode | When | Payer / quote |
|------|------|----------------|
| **Direct attach** | A dials **`media_relay` hop B** directly (stack dialable; no broker) | **A pays B** — N019 quote/ceiling on B ([V022](https://github.com/people-post/pp-browser/blob/main/projects/p2p-av-calls/DECISIONS.md)) |
| **Brokered attach** | A uses **immediate relay R1** for path and/or media (multi-hop, partition, or R1-as-service) | **A pays R1 only** — bundled circuit + subcontracted media + SLA ([N024](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/DECISIONS.md#n024--immediate-relay-as-service-broker)) |

Direct attach remains the simple volunteer / friend-SFU path. Brokered attach is the unified commercial face when R1 orchestrates reachability and downstream `media_relay` capacity.

**Rationale:** Clients need a path without target public IP; friend hops should stay one quote with B; brokered paths need one payer and one SLA owner.  
**Alternatives:** Fail without circuit; always bill B even through R1 (rejected for broker UX — see N024).

---

## H006 — Mobile default Client; call-scoped listen on Wi‑Fi

**Status:** Accepted (updated 2026-08-01 — **N025**)  
**Decision:** Mobile **defaults** to Client: no always-on listen, no always-on `media_relay`, no Node capability UI.

**Planned exception ([N025](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/DECISIONS.md#n025--mobile-call-scoped-listen-on-wifi-not-full-node)):** **Ephemeral listen** on **Wi‑Fi** during a **foreground call** (and optional later **Wi‑Fi helper** toggle) so peers can dial by PeerId on LAN and in-call mobile hops can attach. Scope and relay admission remain **contacts / in-call only** — not public infrastructure.

Idle background reachability still uses **outbound dial + circuit** (and later punch when an introducer Session exists), not persistent mobile listen.

**Rationale:** Role model + battery; scoped listen fixes LAN PeerId dial without full Node.  
**Alternatives:** Mobile temporary hop with no listen gating (rejected — battery/abuse); full mobile Node (rejected).

---

## H007 — No app-layer hop candidate exchange as product path

**Status:** Accepted (updated 2026-09-21 — narrow R1 announce carve-out; amended 2026-10-31 by [H013](#h013--the-callee-advertises-where-it-is-parked) — the callee's parked relays in CallAccept)  
**Date:** 2026-07-31  
**Decision:** Do **not** ship or reintroduce **`call_hop_addrs`** (or similar call-signaling **multiaddr / candidate-list** gather) as the durable hop reachability design. Uncommitted prototypes were removed. Temporary dogfood hacks need an explicit ADR if ever revived. Candidate exchange for punch belongs **in-stack** via introducer Sessions ([H009](#h009--amp-coordinated-punch-acp)), not call signaling.

**Narrow carve-outs:**
- ([H011](#h011--circuit-r1-rendezvous-dialer-authoritative) L3.1c): After the dialer has already chosen an immediate relay, an optional additive **single PeerId** field (`circuit_r1`) may confirm that choice so the answerer can late-reserve. **Forbidden:** lists of candidates, observed UDP endpoints, STUN, or pre-choice hop shopping over call control.
- ([H012](#h012--punch-via-call-signaling-when-no-amp-introducer) B29): When **no Amp introducer Session** is available (circuit not-reg / seeds cannot introduce), a **narrow ACP sync** may ride call-control — same punch semantics as H009, not a second hop-dial toolkit.

**Rationale:** Duplicates what addr book / punch / circuit should do; fights “reachability inside Amp mesh.” One post-ack PeerId is rendezvous confirm, not gather. Signaling punch is only a fallback introducer channel when Amp I is missing.  
**Alternatives:** Keep thin gather until L1 (rejected — prefer document gap + stack work).

---

## H008 — Multi-hop circuit chains (planned)

**Status:** Accepted (plan only — **not implemented**)  
**Date:** 2026-08-01  
**Updated:** 2026-09-04 (parallel to ACP; not a substitute for punch)  
**Decision:** Custom circuit must evolve from **single-hop** to **multi-hop transitive paths** (A → immediate relay **R1** → optional upstream **R2+** → target **B**). Consumer **selects and contracts with R1 only** ([N024](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/DECISIONS.md#n024--immediate-relay-as-service-broker)). R1 chooses upstream relays and (when brokered) subcontracted **`media_relay`** capacity on B.

**Hop limit:** Configurable **`circuit_relay.max_hops`** (default **3** relay legs on path). The stack must **not** hardcode a protocol maximum — honor whatever limit ops/config sets (including &gt; 3). Loop detection and per-hop admission still apply.

**Parallel to punch:** Multi-hop covers **unpunchable** partitions; [H009](#h009--amp-coordinated-punch-acp) covers **punchable** direct paths. Neither blocks the other.

**Rationale:** L3 landed single-hop bridge; real meshes need transitive reachability. Broker model lets R1 own path + media economics and SLA.  
**Alternatives:** Retry only alternate single relays (rejected); consumer pays every hop (rejected); fixed compile-time hop max (rejected); wait for punch before multi-hop (rejected — different failure modes).  
**Spec:** [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md). **Phase:** [L3.5](PHASES.md#l35--multi-hop-circuit-v2).

---

## H009 — Amp Coordinated Punch (ACP)

**Status:** Accepted — implemented (L3.25a–c, [HOLE_PUNCH.md](HOLE_PUNCH.md)); the upgrade-from-circuit helper (`TryUpgradeToDirectAsync`) has no product caller — it is break-before-make and is replaced by call-path-resilience k3 migration  
**Date:** 2026-09-04  
**Decision:** Ship hole punching as **Amp Coordinated Punch**: an introducer that already has authenticated Sessions to both peers exchanges **observed Amp UDP endpoints** and a sync window; both sides simultaneous-dial; first authenticated PeerLink wins under **A026**, with **A027** parent-only teardown for the loser. Prefer seed / Reachable contact / current circuit R1 as introducer (v1). Optional **upgrade-from-circuit** promotes a direct PeerLink then demotes the tunnel.

**Not product paths:** WebRTC/ICE, app STUN, `call_hop_addrs`, libp2p Host DCUtR.

**Preference:** Inserted in [H002](#h002--publish-in-stack--punch--circuit--fail) between publish and circuit.

**Rationale:** After D10/A017 the underlay is Amp UDP; DCUtR-class behavior must be Amp-native. SoftMigrate must not grow a parallel NAT toolkit. Keepalive/`MaybeLearnPath` are not punch.  
**Alternatives:** App ICE gather (rejected — H004/H007); wait forever on UPnP/IPv6 only (rejected — many outbound-only homes); circuit-only forever (rejected — cost/latency); public STUN farm (rejected — second trust plane).  
**Spec:** [HOLE_PUNCH.md](HOLE_PUNCH.md). **Phase:** [L3.25](PHASES.md#l325--amp-coordinated-punch).

---

## H010 — Circuit StartBridge attempt budget (not exhaustive search)

**Status:** Accepted; amended 2026-10-31 by [H013](#h013--the-callee-advertises-where-it-is-parked) — advertised relays first, and the same-relay not-reg retry only on an advertised relay, once  
**Date:** 2026-09-21  
**Decision:** First-connect `EnsureViaCircuit` treats dialable relays as a **ranked queue with a spend limit**, not an exhaustive walk.

| Knob | Value | Notes |
|------|-------|--------|
| Envelope | **14s** (`kCircuitReachEnvelopeMs`) | Covers answerer seed park (~12s) + nest Establish |
| Per StartBridge | **≤4s**, nest slack held until last useful slice | `CircuitStartBridgeTimeoutMs` |
| Max StartBridge calls | **4** | Skips (`!endpoint`, undialable Preferred) do **not** count |
| Nest Establish | **≤8s**, clamped by remaining | After ack only |
| Order | Sticky last-good → Connected → rest | `OrderCircuitRelayAttempts` |
| Same-relay not-reg | **Repeat** until max bridges (no sticky required) | First dual-NAT often has empty sticky (dogfood bb3fbfab) |
| Sticky retry | **Once** on other fast-fail (incl. WaitAck `bridge timed out`) | Hop event-waits far leg |
| Hop ServeDial far-leg wait | **Event-driven** (PeerConnected / reserve / deadline ≤6s) | No Tick poll-resume; deadline is a timer event; **lost-wakeup recheck** after arm |
| Client seed park | **Event-driven** (PeerConnected on bootstrap PeerIds + deadline) | `EnsureBootstrapSeedParkedAsync` — no 250ms poll |
| Parallel StartBridge | **Forbidden** on first connect | Serial only (dogfood ADP path races) |
| Far-leg fail slack | **750ms** before dialer WaitAck | Hop must `fail_near` before dialer opaque `bridge timed out` (dogfood c44e34) |

Answerer remains punch-only + reserve (no reverse StartBridge on first pass). User Retry / TX-only escalate may open a **fresh** envelope later — not a longer first ring.

**Layering (connectivity owns readiness):** Park/reserve and ServeDial far-leg wait live in mesh / `CallMediaPlane` / `CircuitTunnelCoordinator`. Session workflow only speaks consumer needs: `ensure_circuit_ready` (kick) and `await_circuit_ready` (Accept gate) — no seed vocabulary. Sticky not-reg retry stays in `AmpCircuitHopReach` (L3 hop reach).

**ServeDial far-leg wait (event-driven):** When peer-id-only ServeDial has no Connected far leg, hop **arms a waiter** and returns. Resume on Amp `PeerConnectedListener` (PeerId), on live `op=reserve`, or on **deadline event** (`kCircuitServeDialFarLegWaitMs`, capped by tunnel deadline − **750ms** slack). After arm, **re-check** Connected (lost-wakeup between Count and arm). Do **not** poll-retry `BeginServe` from IoTick. Amp: `PeerLinkManager::AddPeerConnectedListener` (multi-listener). Any ServeDial tunnel deadline must `fail_near` (ack) before TearDown — never leave dialer WaitAck to invent `bridge timed out`.

**Client seed park (event-driven):** `EnsureBootstrapSeedParkedAsync` finishes on PeerConnected for a bootstrap/directory PeerId (or deadline). No 250ms poll. Dialer **same-relay** not-reg retries immediately (hop already event-waits); sticky not required. On that retry, dialer re-announces R1 (`OnRelayChosen` / `call_circuit_r1`) so answerer `PreferLateReserve` can re-park (B27).

**Reserve key / re-park (B27):** `op=reserve` is keyed by the protocol-handler PeerId (not a possibly-empty mid-handshake `link.RemotePeerId()`). Empty remote refuses reserve. ServeDial logs `reservation_hit` / `connected_for_target`. Answerer arms a PeerConnected listener on the shared rendezvous surface and re-`StartReserve` when a surface peer reconnects after path change (stale reserve dies with the old ADP link).

**Early circuit-ready (Ringing + Accept gate):** Offerer kicks ready on `StartCall`; answerer on inbound invite; `AcceptInvite` may **await** ready (up to 12s) before `CallAccept`. Hop event wait is the primary race absorber (needs Brief rebuild); Accept await is a thin product backstop.

**Seed park gate:** Before private-Preferred `EnsureAssociation` and again before circuit/punch Ensure, await up to **12s** for bootstrap/directory seeds. Prefer **all** seed PeerIds Connected before finishing early (dialer may StartBridge hop2 while answerer only parked hop1 — dogfood ae4900eb); deadline still accepts ≥1 Connected. A timed-out pre-assoc park must **not** be treated as success (dogfood 88e16f5c). After a successful park, **skip** private-Preferred `EnsureAssociation` (dogfood 39412f). **Answerer** always skips private Preferred when circuit reach is wired. Warm/reserve: connect cold seeds serially without stopping at the first already-Connected; **reserve all** Connected seeds so dialer StartBridge can land on hop2.

**Peer-id-only ServeDial:** Never fall through to dial-book Preferred. Private Preferred hangs hop `EnsureAssociation` until dialer WaitAck `circuit-relay bridge timed out` (dogfood dual-NAT / Windows dialer). Open call-media via `FindLinkByPeerId` + `OpenChannelOnLink` on the live link. Event-wait for Connected far leg; fail `not registered` on deadline so H010 sticky retry can advance.

**Single policy home:** [`CircuitServeDialPolicy.h`](../../../include/mesh/l4/circuit/CircuitServeDialPolicy.h) — shared by hop `BeginServe` / `NormalizeAmpCircuitTarget` and CallMediaBridge seed-park skip (same dual-NAT rule, one header).

CallMediaBridge `kCircuitEnsureBudgetMs` tracks the envelope (~16s with settle slack), not N×20s.

**Rationale:** Directory + DHT + seeds can yield many dialable PeerIds; full WaitAck per candidate blows the connecting window even when ranking is correct. Warm/reserve + short tries beat more candidates.

**Alternatives:** Fixed 20s×N (rejected — dogfood bridge timeout stack); parallel multi-bridge (rejected — UDP path AV); truncate candidate list only without remaining clamp (rejected — still burns on slow misses).

**Code:** `CircuitHopAttemptBudget.h`, `AmpCircuitHopReach::EnsureViaCircuitAsync`.

---

## H011 — Circuit R1 rendezvous (dialer-authoritative)

**Status:** Accepted — **L3.1a–d landed** (shared surface, sticky park, `call_circuit_r1`, hard-w5 STACK); amended 2026-10-31 by [H013](#h013--the-callee-advertises-where-it-is-parked) — the dialer stays the chooser, informed by the callee's parked relays instead of a guessed shared surface  
**Date:** 2026-09-21  
**Decision:** Immediate circuit relay (**R1**) for nested call-media is a **dialer-authoritative rendezvous**, not bilateral hop consensus.

| Party | Role |
|-------|------|
| **Dialer** | Sole selector of R1 (H010 ranked queue + budget) |
| **Answerer** | Parks/reserves a **shared rendezvous surface** covering the dialer’s likely top-K; punch-only on first pass (no reverse StartBridge) |
| **Optional confirm** | After StartBridge ack, dialer may announce **one** `circuit_r1` PeerId so answerer can late-reserve ([H007](#h007--no-app-layer-hop-candidate-exchange-as-product-path) carve-out) |

**Shared surface:** Both sides derive the same ordered PeerId list from mesh eligibility (`BuildCircuitHopList` / dialability filter). Answerer must not use a seeds-only subset that the dialer can walk past. Coverage: all Connected members of the surface, plus enough members to cover **K = `kCircuitMaxStartBridgeAttempts`**.

**Does not change:** H010 spend limits; H008 multi-hop path behind R1; V023 SoftMigrate `media_relay` B pick; session ports (`ensure_circuit_ready` / `await_circuit_ready` stay hop-PeerId-free).

**Rationale:** Independent selection caused dual-NAT not-reg / bridge-timeout races (dialer hop2 vs answerer park hop1). Reserve-all and sticky retry are mitigations; ownership of “final R1” was undefined. Dialer-as-chooser matches H008/N024 “consumer picks one immediate relay.”  
**Alternatives:** Bilateral vote / ICE-style pairs (rejected — H007, complexity); answerer picks and dialer follows (rejected — ServeDial is dialer-driven StartBridge); parallel StartBridge (rejected — H010); exhaustive search (rejected — H010); rely forever on reserve-all luck (rejected — surface asymmetry remains).  
**Spec:** [CIRCUIT_R1_RENDEZVOUS.md](CIRCUIT_R1_RENDEZVOUS.md). **Phase:** [L3.1](PHASES.md#l31--circuit-r1-rendezvous).

---

## H012 — Punch via call signaling when no Amp introducer

**Date:** 2026-09-23  
**Status:** Accepted — **implemented** (L3.25d / B29 client path)  
**Decision:** When Amp Coordinated Punch ([H009](#h009--amp-coordinated-punch-acp)) cannot run because **no introducer Session** exists to both peers (typical: circuit `endpoint not registered` on all seeds — B27), allow a **narrow carve-out of [H007](#h007--no-app-layer-hop-candidate-exchange-as-product-path)**: exchange **ACP punch candidates + sync window** over existing **call-control / relay inbox** signaling, then both sides simultaneous-dial under A026.

| Allowed | Forbidden |
|---------|-----------|
| Observed Amp UDP endpoints already in addr book / dial-back / UPnP (same as H009 collect) | Reintroducing `call_hop_addrs` as SoftMigrate hop shopping |
| Short punch epoch (nonce / window) mirrored from H009 | App STUN / WebRTC ICE (H004) |
| Trigger only after Amp introducer path failed or is unavailable | Using signaling punch as the **primary** path when seeds can introduce |

**Wire:** `call_punch_offer` / `call_punch_answer` (CallControlCodec) carrying candidate multiaddrs + sync window; `AmpPunchCoordinator::TrySignalingPunchBurstAsync` runs BurstDial without an Amp Session to I. Cold-punch exhaust in `CallMediaPlane` → `CallSessionManager::RequestSignalingPunch`.

**Rationale:** Cross-net dogfood (PR #214 B29): ICMP to the right IPv6 answered but punch never burst because introducer needed circuit registration. Signaling already delivers Invite/Accept; it can stand in for I when Amp I is down without inventing a second reachability stack.

**Alternatives:** Wait for B27 relay fix only (rejected — leaves punch dead until seeds work); public STUN farm (rejected — H004); circuit-only forever (rejected — cost/latency).

**Spec:** [HOLE_PUNCH.md](HOLE_PUNCH.md#signaling-introducer-fallback-h012). **Phase:** [L3.25d](PHASES.md#l325--amp-coordinated-punch).

---

## H013 — The callee advertises where it is parked

**Status:** Accepted (2026-10-31)  
**Date:** 2026-10-31

**Context:** Issue #416. The callee parks on at most 2 relays (`kRendezvousLiveTarget`); the caller walks up to
4 of its own 7 in its own order (sticky, then its Connected relays), with no knowledge of the callee's set.
H011's shared surface assumed both sides derive the same ordered list, but the dialer reorders it, the callee
stops at 2 (not K = 4), and each side builds the list from its own directory, DHT and contacts. The fallback
(`call_circuit_r1`, then a late reserve) rides the HTTPS inbox and arrives long after the relay's far-leg
wait (about 3.25 s per StartBridge, not the 14 s envelope). Meanwhile the answerer — Await without punch for
a mobile pair (K013) — gave up about 31 s after answering, while the caller only starts once CallAccept
crossed the inbox (up to 19 s on cellular). Calls `ddc19c94` / `e067fb4e` never tried either relay the
callee held.

**Decision:**
- **CallAccept carries `circuit_parked`:** the relay PeerIds holding a reservation for the callee when it
  answers (at most 4 = the caller's StartBridge cap; relay ids only, never addresses). Read from the mesh's
  published view (`MeshLocalView::parked_relays`), which republishes as reservations change.
- **The caller tries them first,** in the callee's order, ahead of its own ranking (sticky, Connected, the
  refusal demotion). The dialer is still the sole chooser (H011); it is now informed.
- **Same-relay retry on `not registered` only on an advertised relay, once** (its reservation may be
  renewing). A relay the callee did not advertise is advanced past at once: nothing would make the callee
  appear there in time (amends H010's "repeat until max bridges").
- **The answerer waits for the caller** (`await_hold_ms` = 60 s from the start of its reach). The wait ends
  on whichever comes first: a link comes up; the caller sends **`call_connect_give_up`** `{call_id, identity}`
  when its own connect gives up (both screens leave "connecting" together); or the 60 s hold.
- Both additions are additive: old callers ignore `circuit_parked` and old answerers ignore
  `call_connect_give_up` (each keeps today's behaviour); a new caller facing an old callee orders as before.

**Rationale:** The callee knows where it is reachable; the caller can only guess, and every miss costs
3–4 s of a 14 s envelope. Advertising facts the callee already holds is not the candidate gathering H007
forbids (no addresses, no STUN, no pre-choice hop shopping), and the caller learns only which public relays
the callee uses. Aligning the answerer's wait with the caller's real start costs the answerer nothing — on
this path it only listens.

**Alternatives:** Publish parked relays in the mesh (directory / peer record, like libp2p circuit addresses)
— rejected for now: only as fresh as a heartbeat while reservations last 15 s, needs directory-server work,
and publishes the parking set to anyone; a possible later generalisation beyond calls. Deterministic shared
ordering with K = 4 coverage — rejected: double the reservations, still fails when the callee cannot reach
some of the four, still assumes the same directory on both sides.

**Code:** pp-cpp-mesh `PutAdvertisedRelaysFirst` / `CircuitShouldRetrySameRelayOnNotReg`
(`CircuitHopAttemptBudget.h`), `PeerReachRequest::advertised_relays` / `await_hold_ms`,
`CircuitClientCoordinator::ParkedRelays`, `MeshLocalView::parked_relays`; pp-browser
`CallAcceptDetail::circuit_parked`, `CallControlType::CallConnectGiveUp`,
`CallMediaBridge::BuildReachRequest` / `OnPeerGaveUpReaching`.

