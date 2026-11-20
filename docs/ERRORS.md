# Error codes

Every failure a pp-cpp-mesh port reports carries a stable code — callers branch on codes, never on
`message`, which is developer detail (logs, support). The rules are pp-browser's
`docs/contracts/CODED_FAILURE.md`: each **owning** layer has its own `Err` enum
(`CodedFailure<Err>` / `CodedRoe<T, Err>`, pp-cpp-common `common/CodedFailure.h`), wraps only its
**immediate** child's code, and appends the child's message (`detail::AppendFrom`); an **adapter**
port keeps its child's codes as they are.

Module codes stay in the process (never stored or sent); callers use their names. Every enum has
`Ok = 0` first and `Generic` (unclassified) last. What crosses the wire is the string codes below.

## The tree

```text
pp::amp::PeerLinkManager::Err          (pp-cpp-amp; docs/ADR_LINK_PLANE.md)
  └─ IChatPeerLinks::Err               adapter: same values           host/MeshPorts.h
       ├─ IDialRegistry                 adapter: IChatPeerLinks codes  reach/MeshReachPorts.h
       ├─ CircuitClientCoordinator::Err owning: the circuit tunnel     l4/circuit/client
       │    └─ CircuitReachErr          owning: circuit reach (ICircuitHopReach / AmpCircuitHopReach)
       ├─ PunchErr                      owning: one punch              reachability/punch/PunchLinkOps.h
       │    └─ PunchIntroducerWalk::Err owning: the introducer walk    reach/PunchIntroducerWalk.h
       ├─ MediaRelayErr                 owning: quote / attach / send  l4/media_relay/MediaRelayErr.h
       │                                (the client, IMediaRelayClient, the attach flow)
       ├─ DhtClient::Err, DirectoryClient::Err, DialBackErr (below)
       └─ PeerReachCoordinator::Err     owning: reach a peer (dial, circuit, punch)
```

## Codes on the wire

A refusal a peer sends carries a `code` string beside its `error` text. The codes are part of the
protocol; the text is not, and is never read. A refusal without a `code`, or with one this build does not know
(a newer peer's), is a plain failure (`Generic`): `CircuitRefusalFromAck`, `PunchRefusalFromResult`,
`MediaRelayErrFromCode`.

Circuit and media_relay replies, refusals and acks alike, may also carry the node's `load` for the service asked
(`open` / `busy` / `full` / `draining`, docs/PEER_REACH.md § 5a), and `loads` for every service it serves. A `busy` refusal from a node that says `full` or
`draining` is its level speaking: it takes nothing new for that service.

| Protocol | Reply | Codes |
|----------|-------|-------|
| `/pp-mesh/circuit/1.0.0` | `{"ok":false,"code":…,"error":…}` | `target_not_reserved` (the target is not parked here), `target_unreachable` (the relay could not reach it), `busy` (standby refused), `refused` (scope, unknown dialer), `not_ready`, `bad_request`, `bad_version` (before admission) — `CircuitRefusal` |
| `/pp-mesh/reach/punch/1.0.0` | result `{"ok":false,"code":…,"error":…}` | `target_unknown` (the introducer does not know, or cannot reach, the target), `declined` (the target declines this initiator — the introducer passes it on), `no_candidates`, `send_failed`, `bad_request` (also: not JSON, an unknown op, one the phase does not take), `no_path` (no burst path won), `bad_version` — `PunchRefusal` |
| `/pp-mesh/datagram-relay/1.0.0` | `{"ok":false,"code":…,"error":…}` | `busy`, `refused` (scope), `not_ready`, `auth_failed`, `unknown_quote`, `bad_request`, `bad_version` — `MediaRelayRefusal` |
| `/pp-mesh/reach/1.0.0` (dial-back) | `{"v":1,"ok":false,"code":…,"error":…,"dialed":…,"observed":…}` | `bad_request` (not JSON, an op it does not serve), `busy` (a probe from this peer is running), `not_reached` (it ran: no target was allowed or answered), `bad_version` — `DialBackRefusal`. A v9.0.0 seed sends no `code`: `Generic` |
| `/pp-mesh/dht/1.0.0` | `{"op":"error","code":…,"message":…}` | `invalid_json`, `bad_version`, `unsupported_op`, `rate_limited`; a `store`: `missing_record`, `invalid_record`, `not_self`, `expired`, `bad_signature`, `bad_key`, `seq_regression`, `store_failed`, and `bad_version` for a record this node cannot read (it names `record_versions` and `signature_algs`) |
| `/pp-mesh/directory/1.0.0` | `{"op":"error","code":…,"message":…}` | `invalid_json`, `bad_version`, `unsupported_op`, `rate_limited` |

An unknown code (from a newer peer) reads as the protocol's generic refusal, never as its text.

### Versions

Every message names its version: `v` (circuit, media-relay control, dial-back, punch) or
`version` (DHT, directory; and a DHT record's own `version`). A server reads it **first**, before
the op, admission or any crypto, and answers a version it does not read with the code
`bad_version` and `supported_versions` (the versions it reads, `[1]` today), so the asker can say
"update needed" instead of guessing from a failure.

- **Circuit, media relay, dial-back, punch** (V4): only a `v` *above* ours is refused; an absent or
  odd `v` is served as before (v9.0.0 sends 1 and is served exactly as it always was). Media relay
  attached: the message is refused and not applied, the participant stays.
- **DHT, directory** (U7): the check stays strict, as in v9.0.0 (any `version` but 1, or none, is
  refused); the refusal now names the versions.
- **The client side** reads `bad_version` as its layer's `VersionMismatch`: `DhtClient`,
  `DirectoryClient`, `CircuitTunnelErr`, `MediaRelayErr`, `PunchErr`, `DialBackErr`. Downstream:
  circuit reach reads it as `RelayRefused` (and holds the relay back like one); the punch walk asks
  the next introducer; dial-back fails the probe (no reachability answer).
- A v9.0.0 peer ignores the new keys (`code` on dial-back, `supported_versions`): JSON readers on
  both sides skip unknown keys. No protocol id changes.

The punch server answers every frame while it waits for one — an op it does not know, one the
phase does not take (a `sync` before an `offer`), or not JSON — with a `result` coded
`bad_request`, instead of leaving the opener to wait out its 8 s deadline (T10). While it
introduces or bursts, its result is already coming and a stray frame changes nothing.

## Module codes

### CircuitClientCoordinator::Err — one bridge / reserve on one relay

| Code | Meaning |
|------|---------|
| `NotStarted` | the circuit client is not running |
| `BadRequest` | no bridge target |
| `RelayNoEndpoint` | no address for the relay |
| `RelayLinkFailed` | reaching the relay failed (wraps the link code; chain in `message`) |
| `ChannelFailed` | the circuit channel to the relay did not open, or the request could not be sent |
| `Timeout` | no answer from the relay in time |
| `Closed` | the channel or the link to the relay went away before the answer |
| `Aborted` | cancelled by us |
| `BadReply` | the relay's answer could not be read |
| `TargetNotReserved` | relay: the target is not parked on it (`target_not_reserved`) |
| `TargetUnreachable` | relay: it could not reach the target (`target_unreachable`) |
| `RelayBusy` | relay: busy (`busy`) |
| `RelayRefused` | relay: refused us (`refused`, `not_ready`, `bad_request`) |
| `VersionMismatch` | relay: it does not read our circuit version (`bad_version`) |

### CircuitReachErr — reach a peer or a hop through relays (`ICircuitHopReach`)

| Code | Meaning |
|------|---------|
| `Unavailable` | no circuit service (not started, shutting down) |
| `BadRequest` | no peer / hop given |
| `NoRelays` | no relay to try |
| `Aborted` | `AbortPending` |
| `RelayUnusable` | the relays left could not be tried (no address, undialable, the target itself) |
| `RelayUnreachable` | the relays could not be reached |
| `RelayRefused` | the relays refused (busy, scope) |
| `TargetNotReserved` | no relay had the target parked |
| `TargetUnreachable` | the relays could not reach the target |
| `Timeout` | the relays did not answer in time (or the reach envelope ran out) |
| `NestedFailed` | a tunnel came up, the link through it did not |
| `NotConnected` | the steps finished without a link |
| `PunchFailed` | the punch step failed (wraps the walk's code in `message`) |
| `NoCircuit` | an upgrade: no circuit to upgrade |

When several relays were tried, the code is the last relay's. A bridge miss is retried on the same
relay once when it is fast (`CircuitBridgeMissIsFastFail`: the target not reserved or unreachable,
no relay address, no ack in time), and on `TargetNotReserved` the relay is retried before the next
(the answerer may still be parking).

### PunchErr — one punch

`NotStarted`, `EndpointNotRegistered` (no address for the introducer), `InvalidRequest`,
`LinkFailed`, `Timeout`, `ChannelFailed`, `ProtocolError`, `PunchFailed` (it ran; no path won),
`Declined` (the target declines us), `TargetUnknown` (the introducer cannot get to the target),
`VersionMismatch` (the introducer does not read our punch version: `bad_version`).

### PunchIntroducerWalk::Err

The walk asks the next introducer when the punch failed with `TargetUnknown`,
`EndpointNotRegistered`, `LinkFailed`, `ChannelFailed` (this introducer cannot get there) or
`VersionMismatch` (this introducer needs an update); any other failure ends the walk.


| Code | Meaning |
|------|---------|
| `Unavailable` | punch not running |
| `NoIntroducer` | no introducer to ask |
| `IntroducersExhausted` | every introducer was asked; none knew the target |
| `Declined` | the target declines us |
| `NoPath` | the punch ran and no path won |
| `Failed` | the punch failed otherwise (protocol, send, timeout) |

### MediaRelayErr — quote, attach, send

`NotStarted`, `BadRequest`, `HopNoEndpoint`, `LinkFailed`, `ChannelFailed`, `Timeout`, `Closed`,
`Aborted`, `BadReply`, `NotAttached`; the hop's refusals `HopBusy`, `HopRefused`, `AuthFailed`,
`UnknownQuote`, `VersionMismatch` (`bad_version`); and `QuoteDeclined` (the caller's own gate, e.g. a
price).

### DhtClient::Err, DirectoryClient::Err, DialBackErr

`NotStarted`, `EndpointNotRegistered`, `InvalidRequest`, `LinkFailed`, `Timeout`, `ChannelFailed`,
`ProtocolError` (an answer that could not be read, or a refusal by any other code), and:

| Code | Meaning |
|------|---------|
| `VersionMismatch` | the peer answered `bad_version`: it does not read our version (its `supported_versions` are in `message`) — update needed, not misbehaviour |
| `NotServed` (DHT) | the peer serves no DHT: the channel open was refused, no handler |
| `NotFound` (DHT, directory) | someone answered; nobody had it |
| `ConcurrencyLimit` (DHT) | too many lookups in flight |

A DHT publish whose every ask failed `VersionMismatch` or `NotServed` backs off (docs/DHT.md
§ 4.5). A dial-back refusal other than `bad_version` is not a failure: the probe's result says
`ok`:false with its `DialBackRefusal`.

### PeerReachCoordinator::Err — reach a peer

| Code | Meaning |
|------|---------|
| `Unavailable` | no mesh (no lane, no dial registry, the mesh stopped) |
| `BadRequest` | no peer key |
| `Cancelled` | cancelled by the owner |
| `NoEndpoint` | no address to dial and no circuit |
| `DialInBackoff` | the last dial failed recently |
| `DialBusy` | too many dials / links |
| `NoAnswer` | the peer did not answer a dial |
| `Refused` | the handshake failed |
| `TransportFailed` | the OS could not send (the link may be stale — dial fresh next time) |
| `CircuitFailed` | the circuit / punch step failed (`message` has the reach chain) |
| `NotConnected` | the budget ran out without a link and without a more specific failure |

The code is the most telling failure of the attempt: a dial's when the direct dial failed last, the
circuit's when that ran last.
