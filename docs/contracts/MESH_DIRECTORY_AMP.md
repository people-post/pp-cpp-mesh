# Mesh Directory — Amp twin (spec)

> Moved from pp-browser `docs/contracts/MESH_DIRECTORY_AMP.md` at [3e407e3](https://github.com/people-post/pp-browser/blob/3e407e342320719794edd3025c0c7a632bb5f13a/docs/contracts/MESH_DIRECTORY_AMP.md) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)). Links back into pp-browser point at its `main`.

**Tier:** contract (N029 nd4)  
**Status:** Draft v1  
**Date:** 2026-09-02  
**Related:** [N029](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/DECISIONS.md#n029--name-directory-north-star-chain-later-http-now), [PRE_CHAIN_PLAN](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/PRE_CHAIN_PLAN.md), [MESH_DIRECTORY.md](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/MESH_DIRECTORY.md), [SERVICE_ENDPOINTS.md](https://github.com/people-post/pp-browser/blob/main/docs/contracts/SERVICE_ENDPOINTS.md), [AMP-CHANNEL.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md), [MESH_DHT.md](https://github.com/people-post/pp-browser/blob/main/docs/contracts/MESH_DHT.md)

## Purpose

Amp control-channel **phone book** twin of Brief HTTP `ListMeshNodes` / person lookup. Same records as HTTP (`MeshNodeHit` / `NameRecord`). This is **not** DHT PeerId routing and **not** final name authority (chain later).

| Plane | Role |
|-------|------|
| HTTP `/v1/mesh/nodes` | Twin A (Brief / www) |
| Amp `/pp-mesh/directory/1.0.0` | Twin B (mesh-native after L0 seed dial) |
| Mesh DHT | PeerId → multiaddr only — **no names** |

Clients prefer Amp when mesh + bootstrap peers are available, then fail over to HTTP (`FailoverDirectoryClient` / `providers[]`).

## Participation

| Rule | Value |
|------|--------|
| Serve | pp-node / org seed (Node role); answers from local snapshot (self + optional cached listings) |
| Consume | Any mesh client after ADP association to a query peer |
| Default advertise | When `host_directory` (pp-node mesh on; desktop Node optional) |
| Non-goal | Open HTTP CONNECT proxy through Amp |

## Transport

| Field | Value |
|-------|--------|
| `protocol_id` | `/pp-mesh/directory/1.0.0` |
| Channel class | Control (reliable, half-duplex request/response) |
| Framing | One JSON object per DATA frame (UTF-8) |
| Crypto | Outer AMP Session AEAD |

## Wire messages

All messages include:

| Field | Type | Required |
|-------|------|----------|
| `op` | string | yes |
| `req_id` | string (uuid) | yes |
| `version` | int | yes (= `1`) |

### `ping`

Request: `{ "op": "ping", "req_id", "version": 1 }`  
Response: `{ "op": "pong", "req_id", "version": 1, "peer_id": "<local PeerId>" }`

### `list_mesh_nodes` (v1 consumer)

Request:

```json
{ "op": "list_mesh_nodes", "req_id": "…", "version": 1 }
```

Response (success):

```json
{
  "op": "list_mesh_nodes_result",
  "req_id": "…",
  "version": 1,
  "nodes": [ { …MeshNodeHit… } ]
}
```

`MeshNodeHit` fields match HTTP `GET /v1/mesh/nodes` rows: `account_id` (required: a row without one is refused), optional `nickname` / keys, `expires_at`, `entity_kind`, `seq`, `capabilities`, `endpoints[{peer_id, multiaddrs, updated_at}]`.

Response (error):

```json
{ "op": "error", "req_id": "…", "version": 1, "code": "…", "message": "…" }
```

### Person ops (v1)

`search` / `lookup_account` are **not** required on Amp v1. Amp `IDirectoryClient` adapters return failure for person lookups so failover reaches HTTP. Future wire ops may mirror Brief paths.

## Consumer integration

1. Register ADP endpoints for bootstrap / seed peers (L0).
2. Open Amp directory channel to `query_peer_keys`.
3. `list_mesh_nodes` → feed `MeshDirectoryCache` / `INameDirectory::ListService("mesh_node")`.
4. On Amp failure → next `directory.providers[]` entry (typically HTTP).

Config:

```json
"directory": {
  "providers": [
    { "base_url": "12D3KooW…", "transport": "amp" },
    { "base_url": "https://brief.example", "transport": "http" }
  ]
}
```

For `transport: amp`, `base_url` is a PeerId (endpoint key) or ADP multiaddr (PeerId extracted). Product may also auto-prepend Amp against `mesh.bootstrap_peers` when the Amp stack is up.

`CreateOrgBackendClients` (HTTP-only factory) still skips `amp` — Amp backends need `PeerLinkManager` and are owned by `MeshHost` / `ConversationsHub`.

## Hardening (v1)

- Inbound rate limit per remote PeerId (same spirit as DHT window).
- No signed STORE in v1 (server is authoritative for its snapshot).
- Signed directory blobs reserved for later.

## Non-goals

- Fog anycast / epoch manifests
- DHT storing names
- Chain RPC / on-chain registration
- Unpinned DNS dial

## Versioning

| Constant | Value |
|----------|--------|
| `protocol_id` | `/pp-mesh/directory/1.0.0` |
| wire `version` | `1` |
