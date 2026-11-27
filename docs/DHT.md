# Completing the DHT

Status: design agreed 2026-10-21 (decisions D1–D5 as recommended below). Step 6 of
docs/PEER_REACH.md § 7. Progress: § 7 (build order).
Wire and record format: pp-browser `docs/contracts/MESH_DHT.md` (the contract; the record v2 is
§ 4.5b here until the contract follows); this document
says how to make the mesh do what it describes, what changes in it, and what to decide first.

## 1. Where it is

What exists (PEER_REACH.md § 2): signed `PeerRoutingRecord`s (ML-DSA-65, seq, TTL, optional
relay caps), a record store, `ping` / `find_peer` / `store` over `/pp-mesh/dht/1.0.0`, inbound
rate limits, typed STORE rejections, a soft reputation, an outbound concurrency cap. Node role
with `capabilities.dht` only (default off).

What is missing makes it a second directory rather than a DHT:

- no keys or distance — records are keyed by PeerId, there is no XOR metric;
- no routing table — the DHT knows only its query peers (directory nodes + seeds);
- `find_peer` answers from the local store; `closer_peers` is always empty;
- lookup is one round to every query peer; publishing pushes the self record to the same peers;
- the publish gate (reachability ≠ Blocked) and the negative cache are not there.

So a record is found only if the node holding it is one of the seeds / directory nodes the asker
already knows — when those are down or do not hold it, the DHT adds nothing.

## 2. What it is for — and not

For: **PeerId → signed routing record** when contacts, the directory and the seeds do not have
it — (a) the org's infrastructure is down or unreachable (partition escape); (b) more peers than
a few seeds should hold; (c) a source of relay candidates beyond the org, offered only to
consumers that take Public hops (pp-browser now scopes DHT hops Public).

Not for (unchanged from the contract): content or blob routing, pubsub, a relay market, names /
people search (the directory's job), third-party record storage.

## 3. Decisions (agreed)

| # | Question | Decision |
|---|----------|----------------|
| D1 | Complete it now, or after the open reach / error-code items? | After. It is a project of its own (§ 7, ~6 steps + a lab); nothing in calls or chat waits on it — the directory and seeds carry discovery today. |
| D2 | Who looks up? | Nodes with `capabilities.dht` (as now). Clients do **not** walk the DHT: an iterative lookup tells strangers whom you are looking for (your contacts). A Client keeps asking its own seeds / directory. Revisit only with a privacy design (e.g. lookups through a trusted Node). |
| D3 | What does a node store? | Keep the contract's rule: a node STOREs only **its own** record, now at the k nodes **closest to its key** (found by a lookup), not at its query peers. Third-party storage stays out. |
| D4 | How hard against Sybil / eclipse? | Records are signed and carry the signer's public key (`public_key_b64`; the key derives the PeerId), so any node checks a record on its own and a hostile node cannot forge one — it can only withhold or misroute. A record that does not check is dropped, whoever serves it. v1 mitigations: node IDs derived from PeerIds; a routing-table entry only after an authenticated exchange; ≤ 2 entries per /24 per bucket; keep long-lived entries over newcomers; ask α in parallel and accept the best valid record. Disjoint-path lookups (S/Kademlia) are later, if the labs show eclipse is cheap. |
| D5 | Where does liveness come from? | The reach record (PEER_REACH.md § 4): DHT RPCs are dials, so their outcomes land there already. The table evicts entries the record holds down; `ping` only when a full bucket's oldest entry has not been heard from for a while. The soft reputation stays for *misbehaviour* (malformed, expired, wrong records) — reachability is the record's. |

## 4. Design

### 4.1 Keys and distance

`Key256(x) = BLAKE2b-256("pp-mesh/dht/peer-routing/v1\0" ‖ PeerId multihash bytes)` (contract).
A node's ID and its record's key are the same `Key256(PeerId)`. Distance is XOR; bucket index =
the position of the highest differing bit. A peer cannot pick its ID freely — only by grinding
keypairs, which is what the per-bucket subnet cap and long-lived preference make costly to use.

### 4.2 Routing table

256 buckets of up to `k = 20` entries `{peer_id, dial multiaddr, last_heard}`, in memory (not
persisted in v1; rebuilt from the query peers at start).

- **Admission:** only a peer we have exchanged an authenticated DHT RPC with (the Amp session
  proves its PeerId) and that is a DHT participant (it answered `find_peer` / `ping`). A
  `closer_peers` entry is a *hint* to query, never a table entry by itself.
- **Full bucket:** keep the oldest entry if it is alive (heard from lately, or answers a `ping`);
  the newcomer waits in a small replacement list. Entries the reach record holds down leave.
- **Diversity:** at most 2 entries per IPv4 /24 (or IPv6 /48) per bucket.
- **Bootstrap:** the query peers (seeds + directory nodes, via `MeshConnectivity::ChoosePeers`)
  are the first contacts; a lookup of our own key fills the table around us.

### 4.3 Wire (contract changes)

- `find_peer_result.closer_peers`: up to `k_closer` = k = 20 entries from the responder's table
  closest to the target key, `{peer_id, multiaddr}`, never the asker — filled now (always empty
  before). The contract says "≤ α"; that is too few to converge, so it changes to `≤ k_closer`.
  (8 was proposed first; the publish walk showed it too few: with 8, a walk meets only about the
  8–10 closest, never all k it must store at.)
- `find_peer_result` and `pong` say `participant: true|false` (does the responder take part).
  A peer is admitted from its answers only when it says so; one whose `store` is accepted is a
  participant by definition. A table entry's dial address comes from the peer's own signed
  record, or is the address we just reached it at — a hint becomes an address only once an
  authenticated dial to it (the multiaddr ends in `/p2p/<PeerId>`) has been answered.
- No new ops: `find_peer` doubles as FIND_NODE (the asker uses `closer_peers` whether or not a
  record came back); `ping` as is; `store` as is (self record only).
- `version` stays 1: a v1 node sends empty `closer_peers`, which a new node treats as "no hints".

### 4.4 Lookup

Iterative, α = 3 in flight (`DhtLookup`): keep a shortlist ordered by distance to the key; query
the closest not yet asked; add `closer_peers` hints (deduplicated, not held down); stop asking when
a valid record arrives (verified: signature, PeerId binding, TTL, seq) or when the k closest seen
have all answered or failed, and finish once the asks in flight settle — a self answer first, else
the highest seq. Per-RPC timeout as today; the whole lookup bounded (8 s of asking, 30 RPCs). A
lookup that someone answered and nobody had a record for is cached per PeerId for 60 s (negative
cache); one where nobody answered is not (that is our link, not the network). Concurrency cap as
today. Seeds: the query peers, plus — taking part — the routing table's k closest. A Client asks
its query peers only and follows no hints (D2).

### 4.5 Publish

Gate: reachability ≠ Blocked and at least one dialable ADP multiaddr (contract). MeshHost wires
both live (`may_publish`, `current_listen_multiaddrs` — the advertised addresses minus those no
peer can dial); they are looked at no more than once a second. Then: walk to our own key
(`DhtLookup::Limits::WalkToClosest`: past any record, up to 3k asks), STORE our record at the (up
to) k closest that answered — and at the query peers too, because a Client asks only those (D2).
Re-publish at `ttl / 2`, and at once when the addresses change. Not while backgrounded (Nodes are
desktop today).

A publish nobody answered is tried again in 10 s (`kPublishRetrySeconds`) — a network blip. One
that every peer asked *refused* — each answered `bad_version` (it does not read our version) or
serves no DHT (the channel open refused: no handler) — is not cured in 10 s: it backs off, 10 s
then twice as long each time, at most `ttl / 2` (`DhtClient::NextPublishBackoffSeconds`); a
publish someone heard resets it. `publish_backoff_seconds` in the status shows it
(pp-browser projects/release-compatibility T11).

### 4.5a Versions

Every request carries `version` 1 and the server checks it first, before the op and any crypto
(strictly, as v9.0.0 did); a mismatch is `{"op":"error","code":"bad_version",
"supported_versions":[1]}` and the client reads it as `VersionMismatch` (update needed), not a
protocol error (U7). The wire stays at 1; the record inside it is v2 (§ 4.5b).

A record is read in the receiver's order of pp-browser SIGNED_FORMATS § 1.1
(`PeerRoutingRecordFromObject`, then the signature, then `PeerRoutingRecordHasUnknownCritical`;
`ReadPeerRoutingRecord` does all of it for a record checked on its own), each refusal a
`PeerRoutingRecordErr`:

| Step | Refusal | Answered record (client) | `store` (server) |
|------|---------|--------------------------|------------------|
| 1 version | `version` above 2 or not a number: `NewerVersion` | skipped, `records_update_needed` | `bad_version` |
| 1 version | `version` below 2 or none (every v9 record): `Older` | skipped, `records_older` | `bad_version` |
| 2 parse | type, peer_id, multiaddrs, `ext_b64`, a typed blob of id 0 or the wrong size: `Malformed` | soft reputation | `invalid_record` |
| 3 algorithms | the signature's or key's id is not one this build knows: `UnknownAlgorithm` | skipped, `records_update_needed` | `bad_version` |
| 4 signature | does not hold, or the key is not the subject's: `BadSignature` | soft reputation | `bad_signature` (`bad_key`: not the sender's key) |
| 5 extensions | verifies, and carries an unknown critical entry: `UnknownCritical` | skipped, `records_update_needed` | `bad_version` |

Only steps 2 and 4 count against the soft reputation of the peer that served the record: the
others are versions, not misbehaviour (V3). A record's `bad_version` names `record_versions`
(`[2]`) and `signature_algs` (`["ml-dsa-65"]`).

### 4.5b The record (v2)

Since the release-1 cutover (pp-cpp-mesh 10.0.0; pp-browser SIGNED_FORMATS § 2.10, RC020) a
`PeerRoutingRecord` is v2, and v2 is its floor: a v1 record is not read (pre-release cutover,
no reader for older forms).

```json
{
  "type": "peer_routing",
  "version": 2,
  "peer_id": "Qm…",
  "seq": 42,
  "ttl_seconds": 3600,
  "issued_at": 1767225600,
  "multiaddrs": ["/ip4/203.0.113.10/udp/443/adp/2/p2p/Qm…"],
  "capabilities": { "circuit_relay": true, "media_relay": false },
  "ext_b64": "…",
  "signature_b64": "…",
  "public_key_b64": "…"
}
```

- `version` is read first, before any parse or signature check.
- `signature_b64` and `public_key_b64` are **typed blobs** (pp-cpp-crypto `AlgId`): the first
  byte is the algorithm id (signature kind; 1 = ml-dsa-65), then the raw bytes (signature 3310,
  key 1953 bytes). The `signature_alg` field is gone: the signature's id names the algorithm, it
  verifies only with a key of the same id, and the PeerId (key type 4: ML-DSA-65) is derived
  from the raw key, as before. `public_key_b64` is not signed (the PeerId binds it).
- `ext_b64` is the signed extension area's entries (pp-cpp-common `ExtArea`), standard base64;
  absent means empty, and writers omit it when empty. No entry type is defined for the record:
  unknown non-critical entries are ignored and kept (a stored record is served with them, so it
  still verifies), an unknown critical one makes the record "update needed" once it verifies.

The signed bytes (`BuildPeerRoutingSignBytes`): SIGNED_FORMATS § 1.4's header, v1's fields from
`type=peer_routing` on unchanged, and the extension area last. `Len(x)` is a u32 big-endian
byte length, then the UTF-8 bytes; numbers are decimal text inside their field, as in v1.

```text
"pp-mesh:dht-record" 00 ‖ u8 2
‖ Len("type=peer_routing") ‖ Len(peer_id)
‖ Len("seq=" seq) ‖ Len("ttl_seconds=" ttl_seconds) ‖ Len("issued_at=" issued_at)
‖ u32 n ‖ n × Len(multiaddr)
‖ u8 caps_present [‖ u8 circuit_relay ‖ u8 media_relay]
‖ u64 entries_length ‖ entries                       (the extension area)
```

v1 had the domain `pp-mesh:dht-record-v1` 00 and a `Len("sign_version=1")` field where v2 has
the version byte; it had no area. Frozen vectors: pp-specs `vectors/dht_record_vectors.json`
(SIGNED_FORMATS § 6 "Per object": the signed bytes of a fixed input with an empty area and with
one unknown non-critical entry, signed records that verify, and the refusals of the table above),
checked by `dht_record_vectors_test.cpp`; `PP_WRITE_DHT_RECORD_VECTORS=<path>` writes a fresh
file (new hedged signatures, everything else equal).

### 4.6 Refresh

A bucket not heard from for an hour gets one walk toward one of its own contacts' PeerId (a key in
its range, so no new wire op is needed — `find_peer` takes a PeerId); looked for every 10 min, at
most 3 refreshes an hour in total. No other background traffic.

### 4.7 What the consumers see

Unchanged API: `FindPeer(peer_id, cb)` → a record or `NotFound`, now found beyond the seeds. The
pp-browser triggers (prefetch when a contact has no fresh endpoint; partition escape) stay; DHT
hops reach media ranking as Public candidates.

## 5. Tests and the lab

- Unit: Key256 / XOR / bucket index; table admission, eviction, diversity cap; the iterative
  lookup over a simulated network (in-process fake RPC, 200 nodes) converging in O(log n)
  rounds; a lookup through a few lying nodes still returning the valid record.
- Mesh compose: three Amp stacks (existing harness) — publish at the closest, find from the far
  one with no direct query peer holding the record.
- Lab (new, `pp_local_test.sh --suite dht`): ~12 Node containers; the org seeds stop after the
  table fills; every node finds every other node's record; a call between two of them connects
  through a DHT-found address.

## 6. Risks

- **Privacy:** a lookup reveals its target to the nodes asked (D2 keeps Clients out). Records
  publish a Node's addresses to anyone — that is what participation means (opt-in, default off).
- **Load:** Nodes answer strangers' `find_peer`; the inbound rate limits stay, plus the per-lookup
  RPC bound.
- **Eclipse:** v1 mitigations above; the lab measures how many hostile nodes it takes.

## 7. Build order

Status: steps 1–6 done — `DhtKey`, `DhtRoutingTable` (thread-safe, shared by server and client);
the server fills `closer_peers` (never the asker) and admits accepted `store` senders; the client
admits participants that answer and pings a full bucket's oldest when it has been quiet 15 min;
`FindPeer` is the iterative `DhtLookup` with the negative cache; publishing walks to the k closest
behind the gate and follows address changes; stale buckets are refreshed; `reach_held_down`
(the reach record, wired by pp-browser) keeps held-down peers out of walks, stores and the table
(swept every 30 s); the soft reputation counts only malformed or wrongly signed answers (an
expired record is skipped, not held against its server).

1. Key256, XOR, buckets, admission / eviction / diversity — pure, unit-tested.
2. `closer_peers` filled by the server from the table; the table fed by authenticated RPCs.
3. Iterative lookup in `DhtClient::FindPeer` (replaces the one-round fan-out); negative cache.
4. Publish to the k closest; the publish gate; refresh.
5. Soft reputation narrowed to misbehaviour; liveness from the reach record.
6. The lab; contract update (MESH_DHT.md: `closer_peers ≤ k_closer`, admission, publish target,
   `public_key_b64` in the record, `participant` in answers) — done. pp-browser
   `pp_local_test.sh --suite dht` (N-DHT): a seed and 12 Nodes on an internal network, each in its
   own /24, k = 3 (`PP_NODE_DHT_K`); once the tables fill the seed stops, and every Node finds
   every other Node's record (about 110 of 132 by lookup); then N-DHT-CALL: a product-stack caller,
   bootstrapped from a Node and handed a dead address for the callee, finds the callee's record
   and the call connects at that address. `pp_node_dht_smoke.sh` passes again
   (a publish nobody answered retries in 10 s; missing query-peer records are looked up every
   15 s; the reachability probe no longer opens a second link to its seed).
