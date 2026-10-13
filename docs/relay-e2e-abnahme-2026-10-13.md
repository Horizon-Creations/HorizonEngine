# Relay server: end-to-end acceptance (topic 193, step 5), 2026-10-13

What ran, where, with which result, and what is still open. Protocol and
engine design: `docs/relay-server-protocol.md`.

## Result in short

| Leg | Where | Result |
|---|---|---|
| Full build (`cmake --build out/build/release -j8`) | macOS (M-series, Release) | green, rc 0 |
| HE_Net/relay tests against the **real** collab_server (`HE_TEST_RELAY`) | macOS | 27/27 `Relay*` cases, 5/5 `collab tools*` cases |
| Full he_tests run | macOS | 5128/5128 cases, 0 failed |
| 4 editor processes, 1 host + 3 guests, **through the relay** | macOS, relay on 127.0.0.1 | PASS, 51 checks |
| Same 4 editors **without relay** (`HE_NET_RELAY=off`) | macOS | PASS, 43 checks, relay saw nothing |
| Relay on the rented server (212.227.140.164) | — | **not done**: waiting for the human's OK (post 1288) |
| Second platform (Windows/Linux client) | — | **not done**: no client on NN-WS03/NN-RP01 reachable from this step |
| CI (Windows/Linux/macOS build + full ctest) | GitHub | step 4's head (run 38396486313): Linux, macOS, lavapipe green, **Windows red** (MSVC, fixed here, see "Tests"); this step's push starts a new run |

So: unicast and broadcast through a real relay work with several real editor
processes that only dial out, and peer-to-peer works unchanged. What the
step also asked for and this run could NOT show: the relay on the rented
server and a client on a second operating system.

## How it was run

Relay: CollabSession `claude/he-net-relay-server` @ `736694a` (pair protocol v1
plus broadcast fan-out), built into `/tmp` and started as
`collab_server 47901` with its stdout in a log file.

Editors: the deployed Release `HorizonEditor` from this branch, each with a
private HOME and its own copy of the tutorial project, hidden window, driven
over MCP (`HE_MCP_COLLAB_CONTROL=1`, `HE_COLLAB_OFFLINE=1`).

```
scripts/he_collab_relay_e2e.py /tmp/s5_e2e_relay --relay 127.0.0.1:47901 \
    --relay-log /tmp/relay.log --guests 3
scripts/he_collab_relay_e2e.py /tmp/s5_e2e_p2p --p2p --relay-log /tmp/relay.log --guests 3
```

`--relay` sets `HE_NET_RELAY` for every editor, so hosting and joining go
through the relay; `--p2p` sets `HE_NET_RELAY=off`, so the host opens a port
and the guests connect to it directly, as before topic 193. Against a remote
relay drop `--relay-log` (the relay-log witnesses then do not run; the editor
side still does).

For this, `collab_join` (MCP) learned the panel's second path: `sessionId` +
`joinCode` instead of `host` + `port` (`src/HE_Editor/McpToolsCollab.cpp`).
With a relay set, a join by address is refused on purpose (the address does not
say which session), so without it no tool could join through a relay. Tests:
`tests/test_mcp_tools_collab.cpp` ("with a relay set, collab_join goes by
session id", and a real-relay case under `HE_TEST_RELAY`).

## Relay leg: 1 host + 3 guests (PASS, 51 checks)

What was checked, each from the side that did not make the change:

- A hosts through the relay and opens **no listening port** (lsof: no new
  LISTEN socket); every new TCP link of every editor goes **out to the relay
  port** (lsof, compared against the links each editor had before the session).
- B, C, D join by session id + join code, `joined` in 0.1–0.8 s; all four
  rosters name all four people.
- **Broadcast**: A creates an entity, B, C, D see it at A's position.
- **Unicast**: B moves it (lock from the host, 1 `lock_pending` retry), A, C, D
  see the move; C creates an entity, A, B, D see it.
- Guests leave, A's roster is back to A.

Witnesses from the relay log (`relay_log_during_run.txt`):

```
[subscribe session=rxkl5f.. peer=c-5il1snnv9wsk -> host] subscribed to #31 (1 subscribers)
[subscribe session=rxkl5f.. peer=c-0fc39bl2fm2f -> host] subscribed to #31 (2 subscribers)
[subscribe session=rxkl5f.. peer=c-ma6dwfxdls3k -> host] subscribed to #31 (3 subscribers)
[subscribe … peer=c-0fc39bl2fm2f -> host] disconnected after 5.2 s, received 1560 broadcast bytes
[subscribe … peer=c-5il1snnv9wsk -> host] disconnected after 5.3 s, received 1560 broadcast bytes
[subscribe … peer=c-ma6dwfxdls3k -> host] disconnected after 5.0 s, received 1560 broadcast bytes
[listen session=rxkl5f.. peer=host] disconnected after 7.9 s, forwarded 1484 bytes in 19 broadcasts
```

The host handed 19 broadcasts (1484 bytes) to the relay **once**; each of the
three guests received 1560 bytes (the same 1484 plus a 4-byte length prefix per
frame). That is the CPU/upload saving the topic asks for: without the relay the
host would have encrypted and sent each of those 19 messages three times.
Host log: `Relay fan-out: conn 1/2/3 now receives broadcasts through the relay
(n of 3)`.

## Peer-to-peer leg: same 4 editors, `HE_NET_RELAY=off` (PASS, 43 checks)

Same checks. A listens on its session port (62149), each guest has exactly one
new link, directly to that port (`127.0.0.1:62150->127.0.0.1:62149`, …). No
editor logs a relay or fan-out line, and the relay (still running) logged
**nothing** during the run. On the code side: `TcpTransport`, `UdpTransport`
and `SecureTransport` are unchanged over the branch, `RelayTransport` /
`RelayFanout` are only constructed with an address, and "Relay fan-out: without
it, NetSession sends per peer exactly as before" checks bytes and order.

## Tests

All on macOS, Release, `out/build/release`, with collab_server running and
`HE_TEST_RELAY=127.0.0.1:47901`, so the real-relay cases run as well instead of
skipping:

- Full `he_tests` (before the MSVC header fix below): **5128 cases passed,
  0 failed**, 27 skipped (the skips are opt-in live/hardware cases), 1,295,727
  assertions.
- After the header fix: `Relay*`, `collab tools*`, `*Collab*`, `*NetSession*`,
  `*Secure*`, `*Tcp*`: **217 cases, 0 failed**, 87,216 assertions.
- The relay log of the real-relay test cases shows the broadcast path:
  `[listen session=he-fan.. peer=host] … forwarded 694 bytes in 20 broadcasts`.

Windows CI on step 4's head (run 38396486313) was **red**: MSVC
`C2027 use of undefined type 'HE::Net::TcpTransport'` in `RelayFanout.cpp`.
The dllexport of `RelayTransport` makes MSVC define the inline default
constructor in every TU of the DLL, and through `Link` that needs
`~unique_ptr<TcpTransport>` on a complete type; Clang/GCC only instantiate it
where it is used. Fix: `RelayTransport.h` includes `Net/TcpTransport.h`
instead of forward-declaring it (no `windows.h` comes along: `Socket.h` only
uses `<cstdint>`). Mac build green. Whether Windows is green comes from the
next CI run.

## Gaps (not shown by this step)

1. **Relay on the rented server.** My notes say to leave 212.227.140.164
   alone unless asked first, so I asked in topic 193 (post 1288) for permission
   and the open port. No answer during this run. To do: build CollabSession
   there (it is Linux, epoll path; the repo's own CI covers epoll), run it as a
   service, open the port, then
   `scripts/he_collab_relay_e2e.py OUT --relay 212.227.140.164:<port>` from any
   Mac.
2. **Second operating system.** The editors here all run on macOS. A Windows or
   Linux client would need NN-WS03 or NN-RP01 to run either
   `he_tests --test-case='*real collab_server*'` with
   `HE_TEST_RELAY=<relay>`, or an editor from this script, against the same
   relay. I can't reach those machines from this step. The Windows/Linux
   relay code paths are only covered by CI (in-process FakeRelay, no real
   relay, because CollabSession is a private repo and the public HE CI cannot
   check it out without a token).
3. **Game sessions** (`NetGameSession`) through the relay are covered by the
   real-relay test case (host + join with only the join code), not by
   separate game processes.
4. Guest editors whose world the session replaced do not end on SIGTERM
   (unsaved changes) and get killed after 10 s; their block-buffered logs are
   cut off. The script therefore takes the guest-side witnesses from the
   relay log, not from the guests' logs.
