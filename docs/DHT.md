# Completing the DHT

Status: design agreed 2026-10-21 (decisions D1–D5 as recommended below). Step 6 of
docs/PEER_REACH.md § 7. Progress: § 7 (build order).
Wire and record format: pp-browser `docs/contracts/MESH_DHT.md` (the contract); this document
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

- `find_peer_result.closer_peers`: up to `k_closer` (proposal: 8) entries from the responder's
  table closest to the target key, `{peer_id, multiaddr}` — filled now (always empty today). The
  contract says "≤ α"; that is too few to converge, so it changes to `≤ k_closer`.
- `find_peer_result` and `pong` say `participant: true|false` (does the responder take part).
  A peer is admitted from its answers only when it says so; one whose `store` is accepted is a
  participant by definition. A table entry's dial address always comes from the peer's own
  signed record (never from someone else's hint).
- No new ops: `find_peer` doubles as FIND_NODE (the asker uses `closer_peers` whether or not a
  record came back); `ping` as is; `store` as is (self record only).
- `version` stays 1: a v1 node sends empty `closer_peers`, which a new node treats as "no hints".

### 4.4 Lookup

Iterative, α = 3 in flight: keep a shortlist ordered by distance to the key; query the closest
not yet asked; add `closer_peers` hints (deduplicated, not held down); stop when a valid record
arrives (verified: signature, PeerId binding, TTL, seq) or when the k closest seen have all been
asked. Per-RPC timeout as today; the whole lookup bounded (proposal: 8 s, 30 RPCs). A failed
lookup is cached per PeerId for 60 s (negative cache). Concurrency cap as today.

### 4.5 Publish

Gate: reachability ≠ Blocked and at least one dialable ADP multiaddr (contract). Then: look up
our own key, STORE our record at the (up to) k closest that answered, re-publish at `ttl / 2`
and after a network change (new addresses). Not while backgrounded (Nodes are desktop today).

### 4.6 Refresh

A bucket not heard from for an hour gets one lookup of a random key in its range; at most a few
refreshes an hour in total. No other background traffic.

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

Status: steps 1–2 done — `DhtKey`, `DhtRoutingTable` (thread-safe, shared by server and client);
the server fills `closer_peers` (never the asker) and admits accepted `store` senders; the client
admits participants that answer and pings a full bucket's oldest when it has been quiet 15 min.

1. Key256, XOR, buckets, admission / eviction / diversity — pure, unit-tested.
2. `closer_peers` filled by the server from the table; the table fed by authenticated RPCs.
3. Iterative lookup in `DhtClient::FindPeer` (replaces the one-round fan-out); negative cache.
4. Publish to the k closest; the publish gate; refresh.
5. Soft reputation narrowed to misbehaviour; liveness from the reach record.
6. The lab; contract update (MESH_DHT.md: `closer_peers ≤ k_closer`, admission, publish target,
   `public_key_b64` in the record, `participant` in answers).
