# HorizonNet — Networking Layer Design

Status: **Checkpoint N1 (foundation) in progress** — Layer 0–2 abstractions +
in-process loopback transport + serialization, unit-tested. Real transports
(GameNetworkingSockets, WebSocket) and the collaboration/replication consumers
come in later checkpoints.

## Why one layer serves two very different consumers

Gameplay networking and *editor collaboration* (multi-user presence, realtime
file locks, live asset sync for the source-control integration) need the **same
lower half**: a transport, a session, bit-packed serialization, and pub/sub of
state deltas. They diverge only at the top, in *how* state is replicated. So the
architecture shares Layers 0–2 and splits at Layer 3.

```
┌───────────────────────────────────────────────────────────────┐
│  Layer 3a  GAMEPLAY REPLICATION   │  Layer 3b  EDITOR COLLAB    │
│  • NetworkComponent (entt)        │  • Presence                 │
│  • server-authoritative           │  • Realtime locking         │
│  • snapshot + delta compression   │  • change broadcast → pull  │
│  • client prediction / reconcile  │  • co-edit (CRDT, optional) │
│  ↳ UDP, lossy, ~30–60 Hz          │  ↳ reliable, lossless       │
└──────────────┬────────────────────┴───────────┬────────────────┘
               │           SHARED CORE           │
┌──────────────▼─────────────────────────────────▼───────────────┐
│  Layer 2  Messaging & serialization                             │
│  BitStream (bit-packed, quantized) · message framing · dispatch │
├─────────────────────────────────────────────────────────────────┤
│  Layer 1  Session & transport                                   │
│  ITransport (send/poll/disconnect) · NetSession · NetRole       │
│  Backings: Loopback (now) · GameNetworkingSockets · WebSocket   │
├─────────────────────────────────────────────────────────────────┤
│  Layer 0  Platform sockets (shared with the SC HTTPS stack)     │
└─────────────────────────────────────────────────────────────────┘
```

### The honest caveat that shapes the seam

Gameplay and editor collaboration have **opposite** transport requirements:

| | Gameplay (3a) | Editor collab (3b) |
|---|---|---|
| Transport | UDP, loss-tolerant | reliable, **lossless** |
| Frequency | ~30–60 Hz | event-driven, sparse |
| Critical property | latency | consistency / no data loss |
| Topology | listen / dedicated server | central coordinator |

Therefore the **replication models do not merge** — Layers 0–2 + session are
shared; 3a and 3b are separate consumers. Editor sync must not be forced through
the gameplay snapshot/prediction path.

## Why editor collaboration is the *first* consumer

3b needs no prediction, interpolation, or interest management — just transport +
reliable messages + a coordinator. So HorizonNet can be validated by wiring
**presence + realtime locks for the Git/source-control integration first**,
delivering real value before the hard gameplay netcode. Auth is shared with
source control (same OAuth token / OS keychain), and the coordinator can start as
a small managed service — no self-hosting required, consistent with the
Diversion-style backend direction.

This is the concrete tie-in to the source-control work: the "RealtimeLockProvider"
(vs. the LFS-lock and advisory-file-lock providers behind `ILockProvider`) is
simply a Layer-3b module on top of HorizonNet.

## Module layout (`src/HE_Net`, target `HorizonNet`)

Explicit `HE_NET_API` export (like HorizonCore), SHARED lib, dependency-light so
it builds/tests everywhere without GameNetworkingSockets.

| File | Layer | Role |
|---|---|---|
| `include/Net/NetCommon.h` | — | export macro, `ConnectionId`, `MessageId`, `SendMode`, `NetRole`, `NetEventType` |
| `include/Net/BitStream.h` | 2 | header-only `BitWriter`/`BitReader`: bit-packing, quantization, bounds-checked reads |
| `include/Net/ITransport.h` | 1 | transport interface + `NetEvent` (poll-based drain) |
| `include/Net/LoopbackTransport.{h,cpp}` | 1 | in-process cross-wired pair — sockets-free testing + local play-in-editor |
| `include/Net/NetSession.{h,cpp}` | 1½/2 | message framing `[MessageId:16][payload]`, typed dispatch, connect/disconnect callbacks + peer list |

Framing note: the 16-bit message id leaves the stream byte-aligned, so the
payload appends verbatim; a handler reads exactly its typed fields and ignores
any zero-pad in the payload's final byte.

## Checkpoint roadmap

- **N1** *(this)* — Layer 0–2 abstractions + LoopbackTransport + serialization + doctest coverage.
- **N2** — real transport: GameNetworkingSockets (reliable-UDP, encryption, NAT) behind `ITransport`; WebSocket transport for editor/browser. Platform socket layer (Layer 0) shared with the source-control HTTPS stack.
- **N3b** — editor-collaboration MVP: **presence + realtime locking** (`RealtimeLockProvider`), the low-risk first consumer that pays off the source-control locking story.
- **N4a** — gameplay replication: `NetworkComponent` on entt, authority model, snapshot + delta, client prediction / server reconciliation, interest management.

## Cross-platform

Layer 0 socket abstraction covers Winsock (Windows) / BSD sockets (macOS/Linux);
GameNetworkingSockets and WebSocket backings are cross-platform. The loopback
transport is pure STL, so N1 verifies identically on all three OSes. As with the
D3D/Vulkan work, Windows/Linux transports are CI + real-HW verified rather than
blind-merged.
