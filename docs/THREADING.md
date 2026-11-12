# Threading

How a mesh instance uses threads, and what it asks of the application that hosts it. Decided in
pp-browser on 2026-10-19 (its `docs/architecture/THREADING.md` keeps the history and the app's side).

## Lanes

A mesh runs on two kinds of thread:

| Lane | Runs | Given by |
|------|------|----------|
| **I/O** — one serial lane, Amp's driver | Amp, the L4 engines, **and mesh control**: connectivity, reach, rendezvous, reachability steps, media-relay attach orchestration. Timers are Amp timers (`PostAfter`, Amp's clock). | The mesh's runtime, as a `pp::SerialExecutor` (`MeshRuntime::IoExecutor()`): post = `PostToIo`, timers = `PostAfter`, on-lane = `OnDriverThread`; tests pass a `pp::ManualExecutor` |
| **Work** — `work` and `blocking` executors | Off-lane work the mesh hands out: `work` is short and bounded (signature checks, disk, a lock-order hop such as relay quote / attach); `blocking` waits on the outside world (HTTP directory refresh, UPnP discovery) | The host, per mesh instance (`MeshExecutors` in `mesh/types/`, two `pp::Executor`s — an app's lower layers can name them); empty executors run inline (manual-drive harnesses) |

| Amp / mesh dispatch | Through | Examples |
|---------------------|---------|----------|
| Sole driver | `MeshPumpThread` (claims the driver: `MeshRuntime::ClaimDriver`) | `MeshRuntime::Drive` |
| I/O work | `MeshRuntime::PostToIo` | PeerLink mutations, state-machine steps, dial start |
| Deferred teardown | `MeshRuntime::PostDeferred` | Abort, Close, DropLink, L4 `on_done` |
| Timers | `MeshRuntime::PostAfter` | punch window, channel-open deadlines |
| Off the I/O lane | `MeshExecutors::work` / `blocking` | L4 inbound handlers, relay quote / attach, directory refresh, UPnP |

Frame size caps: `pp::amp::AmpChannelLimits`.

**When the pump runs.** `MeshPumpThread` drives when a datagram arrives (it waits on the socket),
when the runtime's next deadline comes (`MeshRuntime::Drive` returns it: timers, resends, the link
walk's keepalives and timeouts, the io ticks'; at most `kMaxIdleWait`, 1 s), and when another
thread's change on the lane is due sooner (`MeshRuntime::SetWakeHandler` → `MeshPumpThread::Wake`:
a post, a sooner timer, a reliable send, or any change while it waits past the 50 ms housekeeping
interval). Mesh io ticks say when they are next due (`AddIoDueTick`): the load sample and the DHT
gate once a second, circuit tunnels and reservations at their deadlines, the media relay while it
keeps sessions; a tick that polls state other events change (a link gone) is run soon after those
events. It waited 5 ms at a time before — 200 wakeups a second on an idle phone (pp-browser #596).
A wait that ends for nothing again and again (a socket "ready" with nothing to read) does not spin:
the pump waits plainly (no poll; at least 5 ms) for a second, and a receive that fails reaches the
host, which checks the socket and reopens a dead one (`UdpSocketCheckReason::ReceiveError`, at most
every `kReceiveErrorCheckGap`).

## Rules

1. **The mesh never calls app code synchronously.** What the mesh needs from the app is **pushed**: the
   app computes snapshots off the mesh's lanes and hands them in (`MeshConnectivity::SetHopPolicy`).
   Candidate policy — contacts, directory, DHT, seeds, trusted relays — is the app's and is computed
   by it; the mesh only reads the snapshot.
2. **What the mesh tells the app is an event**, delivered on the I/O lane; the app's handler must not
   block and moves the work to its own thread. The mesh has no notion of a UI.
3. **Waits go one way: the app may wait on the mesh, never the reverse.** Lifecycle edges (wire,
   rewire, teardown) run on the I/O lane and the caller waits (`SerialExecutor::RunAndWait`); steady
   state is posts and completions. Nothing on the I/O lane waits on anything.
4. **Control lives with its runtime.** Control state is created and freed with the runtime it runs on,
   so its timers and completions never outlive their clock; what must survive a mesh restart is the
   app's input, which it pushes again.

**Why.** Evaluating hop policy on the I/O lane once called into the app — the candidates read the
contacts store (a mutex the UI holds; the first read loads from disk), the directory and the DHT —
which could stall packet handling. The mesh's own control work is all reactions to I/O events and
timers, cheap and non-blocking. Rule 1 removes the callouts, and with them the reason for a second
thread: no hops between control and I/O, one clock for the whole mesh (a `VirtualClock` test covers
control too), and no separate serial lane the library must require of its host.

## Lifecycle edges stay synchronous

The waits left are lifecycle edges only — start, stop and teardown, a rewire on a settings change —
rare and driven by the user or bring-up, never steady-state traffic. Each is bounded: a wait on the
I/O lane holds it for at most a drive turn (~5 ms) plus the edge's own work, and waits go one way
(app → I/O lane), so none can close a cycle. Edges stay `RunAndWait` on the I/O lane — simple for
every embedder.

## Metrics

Each mesh instance records into the `pp::MetricsRegistry` its host gives `MeshHost` (with the
executors). A process with one mesh passes `MetricsRegistry::Global()`; a test can give a host its own
and read only that host's series.
