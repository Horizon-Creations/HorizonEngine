# HorizonNet — Networking Layer Design

Status: **Checkpoints N1 + N2 done, N2.5 partly done** — Layer 0–2 abstractions,
loopback + real TCP transport, an authenticated/encrypted channel, and the
discovery half of N2.5 (UDP, HTTP client, UPnP port mapping, session-directory
endpoint). The directory *client* still needs a TLS-capable HTTP stack (N2.5b).
Session protocol (N3), presence (N4), locking (N5) and live deltas (N6) follow.

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
| `include/Net/Socket.{h,cpp}` | 0 | Winsock/BSD TCP wrapper, non-blocking, `TCP_NODELAY`, async connect |
| `include/Net/LoopbackTransport.{h,cpp}` | 1 | in-process cross-wired pair — sockets-free testing + local play-in-editor |
| `include/Net/TcpTransport.{h,cpp}` | 1 | real network transport; length-prefixed framing over the byte stream |
| `include/Net/SecureTransport.{h,cpp}` | 1 | decorator: challenge-response auth + AES-256-GCM per-frame encryption |
| `include/Net/NetSession.{h,cpp}` | 1½/2 | message framing `[MessageId:16][payload]`, typed dispatch, connect/disconnect callbacks + peer list |

### Why TCP for collaboration

Editor collab needs reliable, ordered, lossless delivery — precisely TCP's
guarantees. A UDP reliability layer (GameNetworkingSockets) buys nothing here and
costs a heavy dependency; it stays reserved for N4a gameplay replication, where
unreliable low-latency channels genuinely matter.

TCP is a byte stream while `ITransport` is datagram-oriented, so frames are
length-prefixed (`[uint32 BE length][payload]`) and partial reads/writes are
buffered per connection. Nothing blocks: sockets are non-blocking throughout, and
`update()` is pumped from the editor frame loop.

### Security model (`SecureTransport`)

Unlike hpak asset encryption (obfuscation only — the key ships with the game),
this **is** a real boundary: a collab session is internet-reachable and carries
the project's scene data.

```
host → client   Challenge  [0x01][version][32-byte hostNonce]
client → host   Response   [0x02][32-byte clientNonce][32-byte mac]
host → client   Accept     [0x03]                    (or Reject [0x04])

mac        = HMAC(secret, "HN-auth-v1" || hostNonce || clientNonce)
sessionKey = HMAC(secret, "HN-key-v1"  || hostNonce || clientNonce)
```

Properties, each covered by a test:

- the join secret never travels on the wire — only an HMAC over both nonces,
- distinct domain-separation labels, so the wire-visible mac reveals nothing
  about the session key,
- fresh nonces per connection, so a captured handshake cannot be replayed,
- a peer is **never surfaced upward as `Connected` until authenticated** — the
  application cannot accidentally talk to an unauthenticated socket,
- data frames are `[8-byte counter][ciphertext || GCM tag]`; the 96-bit nonce is
  `direction || counter`, unique per key, and counters must strictly increase
  (which also rejects replays),
- a failed auth tag drops the link rather than delivering unverified bytes,
- `requireEncryption` (default true) refuses to establish without a crypto
  backend instead of silently downgrading to plaintext.

Crypto primitives are reused from HorizonCore rather than reimplemented:
`Hpak::aesGcm*`, `Hpak::randomBytes`, and `KeyDerivation::hmac` (newly exposed —
dependency-free HMAC-SHA256, so auth works even without a backend).

**The join secret must be machine-generated** (`generateJoinSecret()`: ~128 bits,
Crockford-style base32 without I/L/O/U). A human-chosen password would be
brute-forceable offline by anyone who captures a challenge/response pair.

Framing note: the 16-bit message id leaves the stream byte-aligned, so the
payload appends verbatim; a handler reads exactly its typed fields and ignores
any zero-pad in the payload's final byte.

## Checkpoint roadmap

- **N1** ✅ — Layer 0–2 abstractions + LoopbackTransport + serialization + doctest coverage.
- **N2** ✅ — real network transport: Layer 0 sockets (shared later with the source-control HTTPS stack) + `TcpTransport` + `SecureTransport` (challenge-response auth, AES-256-GCM frames).
- **N2.5a** ✅ — discovery: UDP sockets, plaintext HTTP client, UPnP IGD port mapping, `session-api.php` (register/lookup/heartbeat/unregister with server-side reachability probe).
- **N2.5b** — session-directory *client*: needs HTTPS, which the engine cannot do today (only `OpenSSL::Crypto` / `mbedcrypto` are linked — crypto primitives, no TLS). Options: link `OpenSSL::SSL` / `mbedtls`+`mbedx509`, or use the platform HTTP stacks (WinHTTP / NSURLSession / libcurl). The latter is preferable — certificate validation, proxies and redirects are exactly where hand-rolled TLS goes wrong. Also pending: NAT-PMP/PCP as a second mapping path (needs default-gateway lookup, unlike SSDP which self-discovers).
- **N3** — session protocol: join/leave, participant list, **late-join snapshot** via `SceneSerializer::saveToMemory`.
- **N4** — **presence**: remote cameras as frustum gizmos, remote selection highlighting. First visible collab payoff, no correctness risk.
- **N5** — **authoritative lock table** on the host (`RealtimeLockProvider`), which removes the polling race window LFS locks have.
- **N6** — **live deltas**: dirty-entity replication via `serializeSubtree`, a quantized transform fast path, per-tick coalescing, and UUID-only asset references with a "missing asset" hint.
- **N4a** *(later)* — gameplay replication: `NetworkComponent` on entt, authority model, snapshot + delta, client prediction / server reconciliation, interest management.

## Discovery (N2.5)

How a peer finds a host it has no other way of addressing.

| File | Role |
|---|---|
| `Net/Socket.h` (UDP part) | `socketCreateUdp` / `SendTo` / `RecvFrom` / `socketLocalAddress()` |
| `Net/HttpClient.{h,cpp}` | minimal HTTP/1.1 — **plaintext only**, for LAN router calls |
| `Net/PortMapper.{h,cpp}` | SSDP discovery + UPnP IGD `AddPortMapping` / `GetExternalIPAddress` |
| `Website/HorizonEngine/session-api.php` | session directory: register / lookup / heartbeat / unregister |

**Local address** is found by "connecting" a UDP socket to a public address —
this transmits nothing, it only makes the kernel choose a route — then reading
back the socket's local end. Enumerating interfaces and guessing is unreliable on
machines with VPNs, VMs or several NICs.

**Session directory contract.** The host address is always taken from
`REMOTE_ADDR`, never from the request body: a client-supplied address would let
anyone redirect peers to a host of their choosing. `register` returns a
management token that `heartbeat`/`unregister` require, so knowing a session id
is not enough to drop or hijack an entry. The join secret is never sent to the
directory — authentication is engine-to-engine. Entries expire on a TTL and are
swept on write.

**Reachability is verified server-side** before an entry goes live: the endpoint
opens a TCP connection back to the host. Publishing an unreachable endpoint is
worse than failing, because peers would sit in a timeout with no explanation.

**CGNAT detection.** `PortMapper::isPrivateOrCgnat` flags `100.64.0.0/10` along
with the RFC1918 ranges. A router reporting one of those as its *external*
address proves another NAT layer sits above it, so no port mapping can ever be
reachable — the UI should say so rather than let the user retry forever.

### ⚠ macOS Local Network privacy

From macOS Sequoia on, sends to LAN and multicast addresses fail with
`EHOSTUNREACH` unless the app holds the Local Network permission — while internet
traffic keeps working, which makes it look like a routing bug rather than a
missing entitlement. Measured on this machine: `sendto` to `8.8.8.8` succeeds
while `239.255.255.250` and the LAN gateway both fail. The editor bundle
therefore declares `NSLocalNetworkUsageDescription`
(`scripts/package_macos.sh`), and the user must approve the prompt.

### Verification status — be precise about this

- **Tested**: URL/response parsing (incl. chunked), SSDP message construction and
  `LOCATION` extraction, device-description parsing (the WAN service's *own*
  control URL, not a neighbouring one), SOAP construction with XML escaping,
  CGNAT ranges, UDP round-trip over loopback, and a full HTTP GET against a real
  server socket.
- **Not verified**: SSDP/SOAP against an actual router, because macOS Local
  Network privacy blocks LAN sends from a bare CLI test binary. The pure logic is
  covered, but the live handshake with a real IGD remains unproven — treat it as
  unverified until someone runs it from a granted app bundle.
- **Not verified**: `session-api.php` was never executed (no PHP runtime
  available here). Reviewed only.

## Connectivity over the internet

Two editors behind NAT cannot find each other unaided. The fallback ladder,
best-first:

1. **IPv6 direct** — no NAT at all, only a firewall pinhole.
2. **UPnP / NAT-PMP** automatic port mapping (`miniupnpc` / `libnatpmp`).
3. **Manual port forwarding**, with the editor naming the exact port.
4. **Relay** — later, optional.

Honest limitation: behind **CGNAT** (common on mobile and some ISPs) there is no
forwardable port at all, automatic or manual — only a relay helps. The editor
must therefore verify external reachability *before* publishing an endpoint to
the directory, or clients will find dead entries and time out with no explanation.

Privacy note: the session directory exposes the host's IP to anyone holding the
session id, so ids must be long, random and short-lived.

## Scope boundary: what the live session does *not* carry

Only scene/graph edits, locks and presence replicate live. Asset **bytes**
(textures, meshes, `.hpak`) never do — that is source control's job. Scene deltas
reference assets **by UUID only** (16 bytes); a receiver that cannot resolve one
renders a placeholder and shows a "missing asset" hint rather than pulling the
data over the session. Without this boundary the collab channel silently becomes
a poor file-sync system.

## Cross-platform

Layer 0 covers Winsock (Windows) and BSD sockets (macOS/Linux) behind one API,
including the platform differences in non-blocking mode, error codes, and
`SOCKET` vs `int` handle width. The loopback transport is pure STL, so the upper
layers verify identically on all three OSes. As with the D3D/Vulkan work,
Windows/Linux behaviour is CI + real-HW verified rather than blind-merged —
macOS is the only platform the sockets have actually been exercised on so far.
