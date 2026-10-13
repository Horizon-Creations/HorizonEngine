# HE_Net relay server: protocol and engine mapping

Status (2026-10-13): **server steps 1 and 2 done** (1:1 pairing, broadcast
fan-out), **engine steps 3 and 4 done** (configurable relay address, all links
through the relay when one is set; the host hands messages for many clients
to the relay once). Without a configured relay HE_Net connects peer-to-peer
exactly as before: `TcpTransport` and `UdpTransport` are unchanged, the relay
code is only constructed when an address is set, and `NetSession` keeps its
per-peer send loop for every transport that does not fan out.

## Engine side as built (step 3)

- `HE::Net::RelayTransport` (`src/HE_Net/include/Net/RelayTransport.h`) is an
  `ITransport` that owns one `TcpTransport` connection to the relay per link.
  `RelayTransport::host(relay, sessionId)` keeps the listener connection
  (retried with backoff, 1 s up to 30 s, if the relay is down or restarts) and
  turns each `INCOMING` into an `ACCEPT` connection; `RelayTransport::connect(relay,
  sessionId)` picks a random client id (`c-` + 12 chars) and dials host id
  `host`. `Connected` is emitted only on `paired`; a refusal or close before
  that is `Disconnected` for a link that never connected, and `lastError()`
  carries the relay's reason (`no-such-peer: …`). Sends before `paired` are
  held, not put on the wire. Every `SendMode` is reliable-ordered (one TCP
  stream).
- Address text: `parseRelayAddress("host:port" | "[v6]:port" | "host")`,
  default port 7777. `HE_NET_RELAY=<addr>` overrides every setting,
  `HE_NET_RELAY=off` forces peer-to-peer (`effectiveRelayAddress`). The test
  runner unsets it.
- **Collaboration** (editor): Preferences > Collaboration > *Relay Server
  (host:port)*, stored as `CollabRelayAddress` in the editor config. With an
  address, hosting mints the session id first, registers it on the relay and
  skips port mapping, directory and LAN beacon; guests join by session id +
  join code (a join by address is refused with an explanation). A relay
  refusal fails the join at once with the reason instead of after the 20 s
  connect deadline.
- **Gameplay** (`NetGameSession`): Project Settings > Multiplayer > *Relay
  server*, `multiplayer.relayAddress` in `Config/ProjectSettings.json`,
  reaching `HostOptions/JoinOptions::relayAddress` through the defaults. The
  relay session id is `RelayTransport::sessionIdForJoinCode(code)`
  (HMAC-SHA256 under a fixed label, `g-` + 32 hex), so the existing Join
  dialogs keep working: with a relay the joiner needs only the code, and the
  typed address/port are not dialled (logged). The relay is TCP-only, so a
  game session through it runs on TCP instead of UdpTransport: unreliable
  sends arrive reliably and later under loss, `linkStats()` reports 0. A UDP
  relay would be a server feature of its own. A join the relay refuses fails
  at once with the reason (`lastError()`); without that the session would
  sit in Connecting, because SecureTransport drops a link that dies
  mid-handshake without surfacing it.
- A guest that dials before the host's listener is registered gets
  `no-such-peer`; that is the relay being right, not a race to paper over.
  Tests wait for the registration first.
- Tests: `tests/test_net_relay.cpp`, against an in-process fake relay that
  decodes HELLO by hand, plus one case against a real `collab_server` when
  `HE_TEST_RELAY=host:port` is set.

## Where the server lives

Separate repository, not part of this tree:

- GitHub: `Horizon-Creations/CollabSession` (branch `claude/he-net-relay-server`
  until the queen merges it), on the Mac at `~/VSCode/EngineCollab/CollabSession`
- Binary `collab_server [port]`, default port 7777, C++17 without
  dependencies, epoll on Linux (the rented server), poll() on macOS for tests
- The repository's `README.md` is the authoritative protocol description;
  `tests/relay_test.py` is the executable one (25 end-to-end cases, 10 of
  them broadcast)

### Running instance (rented server)

Since 2026-10-13 the relay runs on **212.227.140.164:7777** (Debian 12, user
`horizon`) as the systemd **user** service `collab-relay` with lingering on, so
it survives logouts and reboots. The account has no passwordless sudo, which
is why it is not a system service. Installed and updated from the Mac with
`deploy/deploy.sh horizon@212.227.140.164 7777` in the CollabSession checkout
(sends the committed HEAD as a `git archive` over ssh and builds it there;
running version in `~/collab-relay/bin/VERSION`).

Stop, restart, logs (on the server, `ssh horizon@212.227.140.164`):

```bash
systemctl --user status collab-relay         # running? since when, which PID
systemctl --user stop collab-relay           # stop (until start or reboot)
systemctl --user start collab-relay          # start again
systemctl --user restart collab-relay        # restart (drops every open session)
systemctl --user disable --now collab-relay  # stop and do not start on boot any more
journalctl --user -u collab-relay -f         # live log, one line per connection event
journalctl --user -u collab-relay --since today
```

From the Mac without logging in:
`ssh horizon@212.227.140.164 systemctl --user restart collab-relay`.
The ssh key `~/.ssh/id_ed25519` has a passphrase; it must be in the agent
(`ssh-add --apple-use-keychain ~/.ssh/id_ed25519`) for scripts to use it.

Port 7777 has to be open inbound twice: in the IONOS cloud firewall policy
and in the host's own packet filter (a ufw ruleset is loaded even though
`ufw.service` shows inactive; `sudo ufw allow 7777/tcp`). The service itself
listens on all addresses (`*:7777`).

## Model in one paragraph

Every relay connection carries exactly one peer-to-peer link. A host keeps one
**listener** connection per session (`LISTEN session/self`). A client opens one
relay connection per host link (`CONNECT session/self/target`). The relay
tells the listener `INCOMING(clientId, token)`. The host then opens a **new**
connection with `ACCEPT session/self/clientId/token`. Both ends get
`STATUS paired`, and from then on the relay forwards raw bytes in both
directions without reading them. Sessions are isolated; peer ids are unique
per session while connected.

Broadcast rides on top of that: the host sends `BROADCAST` frames on its
listener connection, and every client that is **paired with that host** and
has opened one extra `SUBSCRIBE session/self/hostId` connection receives each
payload once. The host sends an update once; the relay makes the copies.

## Wire summary (protocol v1)

Control frames use TcpTransport's framing, `[uint32 BE length][payload]`, and
the payload is `"HERL" | version u8 (=1) | type u8 | fields`. `str8` = u8
length + bytes. Ids are 1..64 chars of `[A-Za-z0-9_-]` (the
`SessionDirectory::newSessionId()` alphabet).

| type | name | fields |
|---|---|---|
| 1 | HELLO (client → relay, first frame) | role u8 (1 listen, 2 connect, 3 accept), session, self, target, token (all str8) |
| 2 | STATUS (relay → client) | code u8, text str8 |
| 3 | INCOMING (relay → listener) | peer str8, token str8 (16 bytes) |
| 4 | GONE (relay → listener) | peer str8: a waiting client gave up |
| 5 | BROADCAST (listener → relay) | the rest of the frame is the payload, up to 64 MiB |

Roles: 1 listen, 2 connect, 3 accept, 4 subscribe (target = host id, no
token; only accepted while `self` is paired with that host in the session).

Status codes: 0 listening, 1 waiting, 2 paired (last control frame, raw bytes
follow), 3 subscribed (last control frame, broadcast frames follow);
refusals, after which the relay closes: 16 bad-hello, 17 bad-version,
18 id-taken, 19 no-such-peer, 20 timeout, 21 full, 22 not-expected.

A subscriber receives each broadcast as `[uint32 BE length][payload]`, which is
exactly `TcpTransport`'s framing; the `HERL|1|5` header is stripped.

Close semantics: if one end closes, the other end gets all pending bytes and
then a FIN. If the listener closes, waiting clients get no-such-peer, existing
pairs keep running, and subscribers get their queued broadcasts and then a
FIN. A subscription ends with the pair it was granted for.

Broadcast rules that matter to the engine:

- No replay: a subscriber gets broadcasts from the next whole frame after it
  subscribed. The initial state has to come over the pair (snapshot), the
  broadcast carries the changes after it.
- No ordering between the pair stream and the broadcast stream (two TCP
  connections). Within the broadcast stream all subscribers see the same
  frames in the host's order.
- Back-pressure: the slowest subscriber pauses reading from the host's
  listener (1 MiB high / 256 KiB low water). A subscriber stuck above the high
  mark for `--stall-timeout` (10 s) is dropped; the others go on.
- A subscriber sends nothing; the relay drops it if it does.

## How the engine side maps onto it (step 3, the design; as built above)

The design goal is that `SecureTransport` and the session protocol run
unchanged over a relay connection. Only the socket setup differs:

- **Client** (`TcpTransport::connect` today): connect to the relay instead of
  the host, send `HELLO connect(session, randomClientId, hostId)`, and wait for
  `paired` before reporting the connection as established. The
  waiting → paired gap must not count as "connected", or the secure handshake
  stalls silently when the host never accepts. A refusal or EOF before
  `paired` counts as a failed connect.
- **Host** (`TcpTransport` listen mode today): instead of a listening socket,
  keep one listener connection to the relay. Every `INCOMING(peer, token)`
  becomes one outgoing `ACCEPT` connection; once it is `paired`, it is
  what `accept()` used to return, so it gets a new `ConnectionId`.
- Session id: reuse the session id HE_Net already has for the session
  (`SessionDirectory::newSessionId()`); the host id can be a fixed name such as
  `host`, since peer ids only need to be unique within the session.
- The relay never sees plaintext: it forwards the bytes after `paired` as they
  are, including SecureTransport's handshake and AES-GCM frames.

## Broadcast on the engine side (step 4, as built)

Stack with a relay: `NetSession → RelayFanout → SecureTransport → RelayTransport`.
`RelayFanout` (`src/HE_Net/include/Net/RelayFanout.h`) is only constructed when
a relay is set (CollabController host/join, NetGameSession host/join).

- **API.** `NetSession::broadcast`, `broadcastExcept(except, …)` and
  `sendTo(conns, …)`. With a transport whose `fansOut()` is true (asked once,
  at construction) the message is framed once and handed to
  `ITransport::sendToMany` once; otherwise it is the same `send()` loop as
  before. One recipient is always a plain send.
- **RelayTransport** gained the raw primitives: `sendBroadcast` (host, a
  type-5 frame on the listener; false while the listener is not registered),
  `subscribe(conn)`/`subscribed(conn)`/`pollBroadcast` (client). A
  subscription the relay refuses leaves the pair alone; one that ends after
  it was granted closes the pair, `lastError()` says "the relay's broadcast
  stream ended".
- **Encryption.** The host draws a random 32-byte group key per session and
  sends it to each client as the first message on its encrypted pair (KEY:
  flags, key, next broadcast index, the client's slot). A broadcast frame is
  `[u64 index][AES-256-GCM(plain) || tag]`, nonce `"HEBC" || index`; the index
  only grows, so a nonce never repeats under one key. The relay sees only
  ciphertext.
- **Order.** Pair and broadcast stream are not ordered against each other,
  and the app above relies on host send order, so the client merges them back
  into it. Every host→client pair message carries `W` = broadcasts issued
  before it and waits until that many are applied. A broadcast plaintext is
  `[u16 n][n × (u32 slot, u64 pair count)][u16 m][m × u32 excluded slot][payload]`:
  for each subscribed client that got pair messages since the previous
  broadcast, how many it has been sent; the client applies the broadcast once
  it has processed that many. A client that got nothing per pair costs no
  entry.
- **Switch.** KEY → the client subscribes → relay `subscribed` → the client
  sends SUBSCRIBED over the pair → from the host's next broadcast on it is on
  the relay. Before that it gets each broadcast per pair as a copy (or a skip
  when excluded) carrying the same index, so a broadcast that also arrived
  over the relay meanwhile is dropped as a duplicate. Every index after KEY
  reaches every client exactly once, and late joiners need nothing special:
  the snapshot goes over the pair and the merge keeps it ahead of the
  changes after it.
- **Except lists.** "Everyone but the sender" (collab's relays of a client's
  edit, presence, transform) is a list of excluded slots in the frame; an
  excluded client advances its index and drops the payload.
- **Loss is loud.** A gap that cannot close drops the link with a reason:
  a subscription the relay ended, a host whose listener was down when it
  broadcast (it drops the clients that were on the relay), or a hold over
  `Config::maxHoldMs` (30 s) or `maxHeldBytes` (64 MiB).

What goes through it: `NetSession::broadcast` everywhere (collab host
presence, components, doc deltas, structural ops, transforms, lock updates,
asset op applies; game despawn and bye), collab's "everyone but the sender"
relays (`broadcastExcept`), and the game's property tables, property deltas
and RPCs to all clients (`sendTo`). **Not** through it: the game's per-client
snapshots (`GameReplication::sendSnapshots` builds each client its own
datagram, interest-culled and with its own input ack, so there is nothing
identical to share) and collab's chunked asset uploads to the other clients
(`sendAssetChunks`, several messages per recipient).

Tests: `tests/test_net_relay.cpp`, the "Relay fan-out:" cases. The FakeRelay
speaks SUBSCRIBE/BROADCAST and can hold either stream to force both
cross-stream orders; one case runs three editors through it, one runs against
a real `collab_server` when `HE_TEST_RELAY` is set.

## Open (later steps of topic 193)

- Access control on the relay itself (today anyone who can reach the port may
  open sessions; the content stays end-to-end encrypted).
- Deployment on the rented Linux server (service unit, firewall), and a client
  on a second operating system. The end-to-end run with several real editor
  processes (relay and peer-to-peer, on macOS against a local relay) is in
  `docs/relay-e2e-abnahme-2026-10-13.md`; the script for repeating it against
  any relay is `scripts/he_collab_relay_e2e.py`.
