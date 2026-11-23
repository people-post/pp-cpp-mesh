# Media hop reachability

> Moved from pp-browser `projects/media-hop-reachability/` at [44bf5c0](https://github.com/people-post/pp-browser/tree/44bf5c0221db8bdd3076b20ab460939c045fda32/projects/media-hop-reachability) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)); its decision ids (H001–H013) are unchanged. Links back into pp-browser point at its `main`.

**Status:** L1–L3 shipped in Amp mesh; L3.25 punch + L3.5 multi-hop planned (parallel); L4 SoftMigrate consume in progress  
**Owner:** Hongwei + agents  

**Related:** [p2p-mesh](https://github.com/people-post/pp-browser/tree/main/projects/p2p-mesh) (relay policy), [NETWORKING.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/NETWORKING.md), [MESH.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/MESH.md), [p2p-av-calls](https://github.com/people-post/pp-browser/tree/main/projects/p2p-av-calls) (V026), [CALLS.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/CALLS.md)

## One-line goal

Peers (and SoftMigrate) can **dial a media hop by PeerId** because the **Amp mesh stack** knows how to find and reach it — address lifecycle, NAT, punch, circuit — not because call signaling invents a second dial toolkit.

## How the pieces fit

```mermaid
flowchart LR
  subgraph stack [This project — Amp mesh]
    Book[Address book]
    Ads[Advertised addrs]
    Punch[ACP punch]
    Cir[Circuit bridge]
  end
  subgraph mesh [p2p-mesh — policy]
    Elig[Eligible hops + scope]
    Quote[Quotes + budgets]
  end
  subgraph calls [p2p-av-calls]
    SM[SoftMigrate]
  end
  Ads --> Book
  Punch --> Book
  Book --> Cir
  Punch --> SM
  Elig --> SM
  Cir --> SM
  Quote --> SM
```

| Question | Where to read |
|----------|----------------|
| How does dial-by-PeerId work? | [DESIGN.md](DESIGN.md) |
| Amp Coordinated Punch | [HOLE_PUNCH.md](HOLE_PUNCH.md) |
| Circuit R1 rendezvous (multi-candidate meet) | [CIRCUIT_R1_RENDEZVOUS.md](CIRCUIT_R1_RENDEZVOUS.md) |
| Multi-hop circuit protocol | [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md) |
| Who may relay, scope, pricing | [p2p-mesh DESIGN](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/DESIGN.md), [RELAY_SCOPE.md](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/RELAY_SCOPE.md) |
| What's in the repo today? | [CURRENT_STATE.md](CURRENT_STATE.md) |
| Build order | [PHASES.md](PHASES.md) |
| Why we chose X | [DECISIONS.md](DECISIONS.md) |

## Ownership (locked)

| Layer | Owns |
|-------|------|
| **Amp + `src/lib/mesh/`** | Discovery, listen/observed addrs, reachability probes, punch, circuit, dial |
| **p2p-mesh policy** | Who may be a hop (contacts ∪ seed), **relay scope / domain bridging** ([RELAY_SCOPE.md](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/RELAY_SCOPE.md)), budgets, incentives, settle; introducer eligibility policy |
| **p2p-av-calls** | SoftMigrate / attach **consume** “is dialable?” |
| **This project** | Spec for the **in-stack** program; thin consume notes |

**Out of scope here:** App-only `call_hop_addrs` / ICE-like gather over chat. Relay scope, bridge score, and scope escalation — [p2p-mesh RELAY_SCOPE.md](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/RELAY_SCOPE.md).

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | **Authoritative spec** — stack model, consume API, ownership |
| [HOLE_PUNCH.md](HOLE_PUNCH.md) | Amp Coordinated Punch plan |
| [CIRCUIT_R1_RENDEZVOUS.md](CIRCUIT_R1_RENDEZVOUS.md) | Dialer-authoritative R1 meet when many relays — H011 / L3.1 |
| [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md) | Multi-hop circuit plan — today single-hop |
| [DECISIONS.md](DECISIONS.md) | ADRs (H001+) |
| [PHASES.md](PHASES.md) | L0–L5 checklists |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Gap vs stack today |
