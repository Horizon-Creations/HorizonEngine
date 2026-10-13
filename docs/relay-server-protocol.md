# HE_Net relay server: protocol and engine mapping

Status (2026-10-13): **server step 1 done** (1:1 pairing, no broadcast yet). The
engine side is untouched; without a configured relay HE_Net connects
peer-to-peer exactly as before.

## Where the server lives

Separate repository, not part of this tree:

- GitHub: `Horizon-Creations/CollabSession` (branch `claude/he-net-relay-server`
  until the queen merges it), on the Mac at `~/VSCode/EngineCollab/CollabSession`
- Binary `collab_server [port]`, default port 7777, C++17 without
  dependencies, epoll on Linux (the rented server), poll() on macOS for tests
- The repository's `README.md` is the authoritative protocol description;
  `tests/relay_test.py` is the executable one (15 end-to-end cases)

## Model in one paragraph

Every relay connection carries exactly one peer-to-peer link. A host keeps one
**listener** connection per session (`LISTEN session/self`). A client opens one
relay connection per host link (`CONNECT session/self/target`). The relay
tells the listener `INCOMING(clientId, token)`. The host then opens a **new**
connection with `ACCEPT session/self/clientId/token`. Both ends get
`STATUS paired`, and from then on the relay forwards raw bytes in both
directions without reading them. Sessions are isolated; peer ids are unique
per session while connected.

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

Status codes: 0 listening, 1 waiting, 2 paired (last control frame, raw bytes
follow); refusals, after which the relay closes: 16 bad-hello, 17 bad-version,
18 id-taken, 19 no-such-peer, 20 timeout, 21 full, 22 not-expected.

Close semantics: if one end closes, the other end gets all pending bytes and
then a FIN. If the listener closes, waiting clients get no-such-peer, and
existing pairs keep running.

## How the engine side maps onto it (step 3, not built yet)

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

## Open (later steps of topic 193)

- Broadcast fan-out (host sends once, relay delivers to all clients of the
  session). With per-pair `SecureTransport` keys a broadcast payload needs its
  own encryption story; that belongs to steps 2 and 4.
- Access control on the relay itself (today anyone who can reach the port may
  open sessions; the content stays end-to-end encrypted).
- Deployment on the rented Linux server (service unit, firewall), plus the
  end-to-end acceptance with real clients (step 5).
