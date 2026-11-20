# The directory row

A directory row (`MeshNodeHit`) is one node's listing: its Account ID (the row's key), endpoints,
capabilities, what it says it runs (N032), and the account's keys. Nodes serve their own rows over
`/pp-mesh/directory/1.0.0` (`DirectoryServer`, the `nodes` of a list answer), `DirectoryClient`
reads them, and the app's HTTP directory serves the same shape. The codec is
`mesh/discovery/MeshNodeHitCodec.h`; `DirectoryHit` (people search, parsed by the app) carries the
same three key fields.

## The keys (since 10.0.0)

Since the release-1 cutover (pp-browser `projects/release-compatibility/SIGNED_FORMATS.md`
§ 2.2–2.3, RC020) the row carries the account's keys as a key card does:

| Field | Holds |
|-------|-------|
| `signing_public_key_b64` | typed blob (pp-cpp-crypto `AlgId`, signature kind): id 1 (ml-dsa-65), then the 1952-byte key |
| `kem_public_key_b64` | typed blob, KEM kind: id 1 (ml-kem-768), then the 1184-byte key |
| `kem_binding_b64` | the KEM binding v2 blob: `u8 2 ‖ ext area ‖ LenBytes(typed signature)` |

`ext area` is pp-cpp-common's `ExtArea` (u64 BE length, then the entries) and `LenBytes` a u64 BE
length, then the bytes. The binding's signature is the account's, over
`"pp-browser:kem-binding" 00 ‖ u8 2 ‖ LenBytes(signing key) ‖ LenBytes(KEM key) ‖ ext area`
(SIGNED_FORMATS § 2.3). v1's `kem_binding_signature_b64` (a bare signature) and `signature_alg`
are gone; a row that still has them is read without them.

## What the mesh checks

The mesh **verifies nothing** in these fields: it carries them for the app, which checks that the
signing key derives the Account ID and that the binding's signature holds. The mesh reads each
field for its shape only, in the receiver's order as far as a carrier can go (version, parse,
algorithms; `DirectoryKeyShape`):

| Shape | When | The field |
|-------|------|-----------|
| `Ok` | a known id with its size; a binding v2 that parses exactly | kept |
| `UpdateNeeded` | a key or the binding's signature of an algorithm id this build does not know, or a binding version above 2 | kept as received; the row's `keys_update_needed` is set |
| `Older` | a binding version below 2 (v1's bare signature reads so) | dropped |
| `Malformed` | not base64, empty, id 0, a wrong size, a binding area or length that does not parse, bytes after the signature | dropped |

The row itself is kept in every case: its endpoints and capabilities do not depend on the keys,
and a dropped field reads to the app as a row without usable keys (the app needs all three).
`keys_update_needed` lets the app say "needs a newer app" for that account instead of "bad keys".
An unknown *critical* entry in the binding's area is not judged here: it is step 5, after the
signature, which is the app's; the mesh keeps the bytes.

## The persisted cache

`MeshDirectoryCache` keeps the last good node list in a file (the app names it,
`mesh_directory_nodes.json`): `{ "schema_version": 2, "nodes": [ … ] }`, each node a
`MeshDirectoryNode` (PeerId, multiaddrs, capabilities, Account ID, N032 claims; no keys).
`schema_version` 2 since 10.0.0: the node shape did not change, but a list saved before the
cutover names nodes on the old signed formats, so a file of any other version is ignored (a
clean start, RC017) and rewritten at the next good refresh.
