# HorizonNet — Networking Layer Design

Status: **N1–N6 done, plus editor integration.** Transport, forward-secret
authenticated encryption, discovery through a live session directory, the session
protocol with chunked late-join snapshots, presence, an authoritative lock table,
and live replication of transforms, authored assets and structural changes — all
reachable from the editor (View ▸ Collaboration) with per-user undo/redo.

Gameplay replication (N4a) has its foundation: `NetworkComponent`,
server-authoritative snapshots, interest management, interpolation. Client-side
prediction and reconciliation are not implemented, so a locally controlled
character lags by the round trip.

Not done: NAT-PMP as a second port-mapping path. Not verified: two real editor
instances in one session, which needs two GUI processes.

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
host → client   Challenge  [0x01][version][32 hostNonce][32 hostPub]
client → host   Response   [0x02][32 clientNonce][32 clientPub][32 mac]
host → client   Accept     [0x03]                        (or Reject [0x04])

transcript = hostNonce || clientNonce || hostPub || clientPub
mac        = HMAC(joinSecret, "HN-auth-v2" || transcript)
shared     = X25519(ourEphemeralPriv, peerPub)
prk        = HMAC(key = shared, "HN-key-v2" || transcript)
sessionKey = HMAC(key = prk,    joinSecret)
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
- **N2.5b** ✅ — session-directory *client* over HTTPS, with TLS delegated to the platform (`HttpsClient_Apple.mm` / `_Win.cpp` / `_Curl.cpp`) and `SessionDirectory` on top. Still pending: NAT-PMP/PCP as a second mapping path (needs a default-gateway lookup, unlike SSDP which self-discovers).
- **N3** ✅ — session protocol: join/leave, participant list, **chunked late-join snapshot** behind `ISessionStateProvider`.
- **N4** ✅ — **presence**: camera pose + selection per participant, throttled and relayed by the host.
- **N5** ✅ — **authoritative lock table** on the host, which removes the polling race window LFS locks have.
- **N6** ✅ — **live deltas**: transforms, all other components, authored assets, and structural changes (create/destroy/reparent) replicate, with lock-derived authority.
- **N4a** ◐ — gameplay replication: `NetworkComponent`, server-authoritative snapshots at a fixed tick, quantized transforms, interest management and client-side interpolation. Prediction/reconciliation still missing — see below.

## Discovery (N2.5)

How a peer finds a host it has no other way of addressing.

| File | Role |
|---|---|
| `Net/Socket.h` (UDP part) | `socketCreateUdp` / `SendTo` / `RecvFrom` / `socketLocalAddress()` |
| `Net/HttpClient.{h,cpp}` | minimal HTTP/1.1 — **plaintext only**, for LAN router calls |
| `Net/PortMapper.{h,cpp}` | SSDP discovery + UPnP IGD `AddPortMapping` / `GetExternalIPAddress` |
| `Net/HttpsClient.h` + `_Apple.mm` / `_Win.cpp` / `_Curl.cpp` | TLS-capable HTTP, **delegated to the OS** |
| `Net/SessionDirectory.{h,cpp}` | register / lookup / heartbeat / unregister client |
| `Website/HorizonEngine/session-api.php` | the directory endpoint itself |

### TLS comes from the operating system

| Platform | Backend | TLS underneath |
|---|---|---|
| macOS | NSURLSession | Secure Transport / system trust store |
| Windows | WinHTTP | Schannel |
| Linux | libcurl | distro TLS + CA bundle |

Nothing in the engine implements TLS. Hostname matching, chain building, expiry,
revocation, the system trust store and corporate proxies are where hand-rolled
TLS fails — and it fails *silently*, staying invisible until someone is actually
attacked. The platform stacks already solve it and track OS policy updates.
HorizonCore links only the crypto *primitives* (`OpenSSL::Crypto` / `mbedcrypto`);
those are not a TLS implementation.

On Linux without libcurl the build stays green but `httpsAvailable()` reports
false and every directory call fails with an explicit message — the feature is
unavailable rather than quietly broken, and the UI must say so.

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
- **Tested live**: the macOS TLS path, against real endpoints — a valid
  certificate returns 200, while expired, self-signed and wrong-hostname
  certificates are all *rejected*. Certificate validation is therefore
  demonstrated, not merely asserted. Parser-level tests alone would not have
  shown this.
- **Not verified**: SSDP/SOAP against an actual router, because macOS Local
  Network privacy blocks LAN sends from a bare CLI test binary. The pure logic is
  covered, but the live handshake with a real IGD remains unproven — treat it as
  unverified until someone runs it from a granted app bundle.
- **Not verified**: `session-api.php` was never executed (no PHP runtime
  available here). Reviewed only — and never deployed.
- **Not verified**: the Windows (WinHTTP) and Linux (libcurl) TLS backends have
  never been compiled or run; only the Apple one has. They need CI.

## Session protocol (N3)

`Net/CollabSession.{h,cpp}` — the first real Layer-3b consumer.

```
client → host   JoinRequest      protocol version, display name
host  → client  JoinAccepted     assigned id, state sequence, participant list
                (or JoinRejected: VersionMismatch / SessionFull / SnapshotFailed)
host  → client  SnapshotBegin / SnapshotChunk… / SnapshotEnd
host  → others  ParticipantJoined
…on disconnect  ParticipantLeft
```

**Layering.** `CollabSession` deliberately does *not* depend on HorizonScene.
HorizonNet sits below the scene layer, so reaching up into it would invert the
dependency and drag the ECS into every networking test. Snapshot capture/apply
goes through `ISessionStateProvider`, which the editor wires to
`SceneSerializer::saveToMemory` / `loadFromMemory`; the tests supply a trivial
in-memory provider, so the whole protocol is exercised with no ECS in sight.

**Chunked snapshots.** A scene can be many megabytes. Sending one blob would give
no progress feedback and force the receiver to accept an arbitrary allocation up
front, so the transfer is split (default 256 KB) with begin/chunk/end framing and
a progress callback. The receiver refuses an announced size above its own limit,
refuses a chunk that would exceed the announced total, and refuses to apply an
incomplete transfer — a partially deserialized scene is worse than none.

**Ordering decisions that matter:**

- The host captures its state *before* admitting a joiner. If capture fails the
  join is refused, rather than admitting a peer that believes it is in sync while
  holding an empty scene.
- A client is not `isJoined()` until the snapshot has been **applied**. Reporting
  success at `JoinAccepted` would let the editor act on a scene it does not have.
- The join request is sent once, not every `update()`; a repeat would allocate a
  second participant slot for the same peer.

**`stateSequence()`** is carried through the snapshot even though nothing
increments it yet: live deltas (N6) continue from it, so defining it now avoids a
protocol break later.

## Presence (N4)

Per-participant volatile state — where someone is looking and what they have
selected — relayed by the host. Unlike the scene itself it is disposable: a lost
update is corrected by the next one.

**Identity is stamped by the host, never claimed by the sender.** The
client→host message deliberately carries *no* participant id; the host derives it
from the connection the frame arrived on and only then relays it with an id
attached. Two message ids exist for exactly this reason — a single shared layout
would let a client publish presence on someone else's behalf, moving another
user's camera gizmo or faking their selection.

**Throttling is the other half of the design.** A gizmo drag produces a change
every frame, for state that is stale a frame later. Presence is therefore sent at
most every 100 ms (10 Hz) *and* only when something moved beyond an epsilon, so
an idle editor emits nothing at all. Both are covered by tests: 60 frames of
continuous movement inside one interval produce at most two messages, and a
stationary camera produces none.

`update(nowMs)` takes the time explicitly. Reading a wall clock inside would make
throttling untestable and irreproducible, and the explicit form also suits a
fixed-step host loop.

**Encoding.** Position stays full float precision — a scene can span kilometres,
so a fixed quantization range would either clip or lose centimetres. Rotation is
a unit quaternion, so every component is bounded by [-1, 1] and quantizes cleanly
to 16 bits (~3e-5 of angular resolution, far finer than a gizmo can show) at a
quarter the size. Selection ids are opaque 64-bit values: HorizonNet has no idea
what they refer to, which is what keeps this layer independent of the scene.

Presence is dropped when a participant leaves, or their camera gizmo would linger
in everyone's viewport forever.

## Locks (N5)

The host holds the only copy of the lock table and arbitrates every request.
That is the whole difference from Git-LFS locks: there, each client *polls* a
server, so two people can believe they hold the same lock for a few seconds.
Here a request is answered by the one authority, in order, with no window at all.

- The subject is an **opaque 64-bit id**, like presence selection — entity,
  hashed asset UUID, whatever the editor decides. HorizonNet never interprets it.
- The host answers its own requests locally, with no round trip.
- A grant is broadcast to *everyone including the requester*: the broadcast **is**
  the confirmation, so there is no second "you got it" message that could drift
  out of sync with it.
- **Only the holder may release.** Otherwise anyone could free someone else's
  lock and start editing underneath them.
- Re-requesting a lock you already hold is a no-op, not a denial — otherwise
  re-selecting the same object would report a conflict with yourself.
- **A joiner receives the current table**, or a late arrival would believe
  everything is free and collide immediately with whoever is already editing.
- **A departing participant's locks are freed** — before they leave the roster,
  so the broadcast still reaches everyone — or their locks would block the
  session forever.

## Presence visualization (E2)

Remote cameras are drawn as small frustums and remote selections as coloured
boxes, both through `DebugDrawBuffer` — overlay lines only, never scene state,
which is what keeps presence the low-risk half of collaboration.

Colours come from `CollabController::participantColor`, stepping hue by the
golden ratio so consecutive participant ids land far apart on the wheel: two
people who joined one after another never get near-identical gizmos. It is
derived from the id, so every peer picks the same colour for the same person
without having to agree on one.

## Live scene deltas (N6, v1)

`CollabSession::TransformDelta` — one entity's transform, keyed by its **entity
handle**. Handles match across peers because everyone started from the same
snapshot, so no id remapping is needed.

Rotation is Euler degrees, matching `TransformComponent` exactly. Quantizing it
to [-1,1] the way presence does would be wrong: that is only valid for a *unit
quaternion*, and Euler angles legitimately run past 180°. Position, rotation and
scale therefore all go at full float precision — unlike a presence gizmo, this is
the authoritative value.

**Authority comes from the lock**, which is why N5 had to land first: only the
holder may move a subject, so there is no conflict to resolve and no merge
strategy to get wrong. The host **re-checks** ownership on arrival rather than
trusting the sender — a test hand-builds a frame that bypasses the local guard
and confirms it is dropped.

Sending is rate-limited (~30 Hz) and skipped entirely when nothing moved, for the
same reason as presence: a gizmo drag produces a change every frame.

### Structural changes — network ids, not handles

Creating, destroying and reparenting entities replicate too, keyed by a **network
id** rather than a raw ECS handle:

- entities from the shared snapshot use their handle — identical on every peer,
  since everyone deserialized the same bytes into a cleared world,
- newly created ones use `(participantId << 32 | counter)`, which cannot collide
  with a snapshot handle (those have a zero high word) nor with another
  participant's ids.

**Why not handle-stable instantiation instead?** It was considered and rejected.
`entt::create(hint)` only honours the hint when that identifier is free, and in
this engine every peer independently creates *local-only* entities — terrain
chunks regenerate from `TerrainComponent`, environment lights belong to the Sky
entity. Their handle allocators are therefore never in lockstep, so "the same
handle" would stop meaning "the same entity" the moment terrain exists. It would
work *most* of the time, which is the worst kind of failure. The id map makes
identity explicit and lets `instantiatePrefab` mint whatever handle it likes.

Those same local-only entities are excluded from replication: each peer makes its
own, and sending them would duplicate them on arrival.

**Detection is by diffing the entity set**, not by hooking edit sites. The editor
creates and deletes entities from many places — outliner menus, drag & drop,
prefab drops, terrain tools — and hooking each one would inevitably miss some. A
diff is complete by construction.

Authority: creating needs no lock (a brand-new entity cannot be held by anyone),
but destroying and reparenting do — otherwise an entity could be yanked out from
under whoever is editing it. Destroying also frees the subject's lock, or the
table would keep blocking something that no longer exists.

### Component edits

Everything a transform delta does not carry — mesh and material assignments,
lights, cameras, colliders, scripts, names — replicates as the entity's
**serialized component state**, via `SceneSerializer::serializeEntityComponents`
and `applyEntityComponents`. Those reuse the same restore path as scene loading,
so a component added to the scene format replicates with no extra work here.

Applying updates an *existing* entity rather than creating one: a prefab blob
would mint a new entity, which is the wrong operation when the peer already has
it and is merely editing it.

Sent as a whole blob rather than a field diff. The payload is one entity's
components — small — and a diff would need a shared schema on both ends. The lock
is what makes a wholesale overwrite safe: nobody else can be editing that entity,
so nothing of theirs can be lost.

Change detection costs almost nothing because **only the held entity can have
changed**: you may edit only what you hold, so the editor serializes that one
entity per frame and hashes the result. An entity that is merely selected rather
than edited sends nothing.

## Undo in a shared session — the problem, and a proposal

**Undo as it exists today cannot be made multi-user safe.** `EditorUndo` is
snapshot-based: every entry is a full-world CBOR blob, and undo *restores the
whole world*. In a session that would silently revert everyone else's concurrent
work — not just your own. There is no way to fix that by adjusting when undo
fires; the data model itself is wrong for the job.

Today the editor sidesteps it: a received delta is applied **without** going
through the undo system, so at least a remote edit never lands on your stack.
That leaves the reverse hole open — your own undo still restores a whole-world
snapshot taken before their edits existed.

### Proposal: inverse-delta undo, scoped by lock

The delta work already gives us the missing ingredient. An undo entry becomes an
**inverse operation** instead of a snapshot:

1. Before applying a local change to subject *S*, capture its previous value.
2. Push `(S, before, after)` onto a per-user stack.
3. Undo = send the *inverse* delta (`S` back to `before`) as an ordinary change.
   Everyone applies it like any other edit; nothing is "restored".

That makes undo an edit like any other, which is what makes it composable with
other people's work. It is how Figma and collaborative editors solve this.

**Locks make it sound.** You may only edit what you hold, so while an entry is on
your stack nobody else can have touched that subject — inverting it cannot
clobber anyone. The rule that follows: **an undo entry is only valid while you
still hold the lock on its subject.** Release the lock (deselect), and entries
for that subject are dropped from your stack, because someone else may have
changed it since and your "before" value is no longer a truthful inverse.

**Scope and cost.** This only covers what deltas cover — transforms today. Until
structural replication exists, create/delete cannot be undone this way, so in a
session those operations should be blocked or confirmed rather than silently
half-undoable. Outside a session, snapshot undo stays exactly as it is: it is
simple, it works, and there is no reason to pay for operation-based undo when
you are alone.

### Implemented — `CollabUndo`

The stack now exists and is wired into the Edit menu, so **everyone undoes and
redoes their own changes**. It covers both kinds of change that replicate:
transforms and authored assets.

- Undo re-applies `before` and **publishes it as an ordinary edit**; redo does
  the same with `after`. Nothing is restored, so it composes with other people's
  work.
- Recording clears the redo branch, as any undo stack does.
- **Entries die with the lock.** `dropUnowned()` runs before every undo/redo, so
  once you release a subject its entries vanish — someone else may have changed
  it since, and the recorded `before` would overwrite their work rather than
  undo yours.
- A transform entry spans a whole edit, not one per frame: the editor keeps a
  baseline for the held subject and records the delta against it.
- An asset with no previous state (freshly created) cannot be undone — that
  would mean deleting the file, a different operation than this stack models.
- Menu labels name the change ("Undo change to MainMenu.huiw") rather than a bare
  "Undo", which tells the user nothing when several kinds of edit are in flight.

Still limited to what replicates: structural changes (create/delete/reparent)
are neither synced nor undoable this way.

## Authored-asset sync

Every authored asset — HorizonCode graphs, materials, material functions, UI
widgets, particle and animator graphs, input assets, scripts and scenes — travels
over the session as a **chunked byte blob**, keyed by a hash of its project
path. One mechanism covers all of them, without a line of per-type code, because
they all funnel through `ContentManager::saveAsset`.

That is also the hook: `setOnAssetSaved` is a pure notification, so HE_Core stays
unaware that collaboration exists. Receivers write the bytes verbatim and reload,
rather than deserializing per type — which is what makes it uniform.

**Binary media is deliberately excluded.** Meshes, textures and audio do not
travel this way: they are large, almost never edited during a session, and belong
to source control. Pushing them through the session would quietly turn it into a
poor file-sync system — the same boundary drawn at the very start of this design.

Authority is the lock table again: publishing an asset requires holding its
subject, the host re-checks on arrival, and a save is refused outright when
someone else holds it. Applying a received asset sets a guard flag, so writing it
to disk does not bounce straight back out through the save notification.

## Do private hosts need a TLS certificate? No — and here is why

There are two separate encrypted surfaces, and conflating them causes needless
worry:

| Surface | Who proves identity | Certificate needed by the host? |
|---|---|---|
| Engine ↔ session directory (website) | the *website's* certificate | **No** — the host is only a client |
| Engine ↔ engine (host ↔ peers) | the shared join secret | **No** — this path uses no TLS at all |

The peer link is `SecureTransport`: a pre-shared join secret, an HMAC
challenge-response, and AES-256-GCM. No CA, no domain name, no certificate —
it works on a bare, dynamic IP address.

**This is the right construction, not a shortcut.** PKI answers a question we do
not have: *"I am contacting a stranger claiming to be example.com — prove it."*
That needs a domain and a third party to vouch for it. Here, both sides already
share a join code passed out of band (chat, voice, screen share). When a secret
is already shared, a pre-shared key is simpler *and* stronger: no certificate
authority has to be trusted at all. WireGuard and Syncthing are built on the same
reasoning.

A certificate would also be impractical for a private host: certificate
authorities issue for domain names you control, home users have none, addresses
are dynamic, hosts usually sit behind NAT, and public CAs do not issue for
private IPs. Running ACME renewals for a session that lasts an afternoon is
absurd.

**Self-signed certificates would be strictly worse.** They look like TLS while
having no trust anchor, so there is nothing to validate against — you would have
to pin a fingerprint exchanged out of band, which is precisely what the join code
already does, with less machinery. Worse, it trains users to click through
certificate warnings.

### Forward secrecy — closed in handshake v2

The original v1 handshake derived the session key deterministically from the join
secret and two public nonces. Anyone who recorded the ciphertext and obtained the
join code *later* could decrypt that recording retroactively.

v2 roots the key in a per-connection **X25519** exchange instead. Both ephemeral
private scalars are wiped as soon as the shared secret is computed, so they exist
nowhere afterwards — a leaked or reused join code no longer unlocks past traffic.

Two properties are deliberately combined:

- The join secret is folded into the **final** key, so breaking the curve alone
  does not yield it either. Neither input is sufficient on its own.
- The mac covers the transcript **including both public keys**, so a man in the
  middle cannot substitute a key it controls — it cannot produce a valid mac
  without the secret. This is verified by a test that flips a byte inside the
  ephemeral key in flight and confirms no session is established.

`HE::Crypto::x25519*` (HE_Core) wraps OpenSSL's `EVP_PKEY_X25519` and mbedTLS's
PSA `psa_raw_key_agreement`; neither implements the curve here, so clamping and
constant-time arithmetic come from the library. Both backends are pinned to the
**RFC 7748 §6.1 known-answer vectors**, so they are measured against the spec
rather than merely against each other — a consistently wrong implementation would
pass a self-consistency check.

`sessionFingerprint()` exposes a one-way 64-bit digest of the negotiated key
(never the key). Both peers compute the same value, so it can be shown in the UI
to confirm out of band that two users are really in the same session.

**What the tests can and cannot show.** They prove the mechanism is present and
per-session: the transcript carries 32-byte ephemeral public keys, both sides
contribute one, and they differ across sessions. Forward secrecy itself follows
from those private scalars being discarded — the absence of a derivation is a
design property, not something a unit test can demonstrate. Note that "each
session gets a different key" alone proves nothing here: v1 satisfied that too.

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


## Gameplay replication (N4a)

`HorizonScene/GameReplication` + `NetworkComponent`. Deliberately a *separate*
consumer from editor collaboration despite sharing the transport, because the two
want opposite things: collab replicates authored edits — rare, reliable, must
never be lost — while gameplay replicates simulation state at ~30 Hz and tolerates
loss, since a dropped snapshot is corrected by the next one milliseconds later.
Forcing gameplay through the collab path would make every position update a
reliable message; forcing collab through this one would silently drop an edit.

- **Registration is opt-in.** An entity without a `netId` is not replicated,
  which is how purely local effects (muzzle flashes, debris) stay off the wire.
- **Clients never mint ids.** `adoptEntity` takes the id the server assigned —
  otherwise the two peers would disagree about which entity a snapshot names.
  (This API was added because a test exposed its absence.)
- **Interest management** is the single largest bandwidth lever, far more than
  per-field compression: entities beyond a client's relevance radius are never
  sent to it at all.
- **Quantization**: 24 bits per position axis over ±4096 (sub-millimetre), 16
  bits per Euler angle (~0.005°) — about 19 bytes per entity per snapshot.
- **Unreliable by intent.** Waiting for a retransmit would deliver state that is
  already wrong.
- **A stalled server sends one snapshot, not a backlog** — catching up would
  burst updates that are all stale on arrival.
- **Interpolation** between the last two snapshots keeps other entities from
  visibly stepping at the tick rate on a higher-refresh display.

### What is missing, and why it matters

**Client-side prediction and server reconciliation.** Without them a player's own
character responds only after a full round trip, which feels wrong at any ping
above ~50 ms. This is the honest foundation, not finished netcode.

**Euler interpolation is wrong across the ±180° seam** — a quaternion path fixes
it, and belongs with the prediction work rather than before it.
