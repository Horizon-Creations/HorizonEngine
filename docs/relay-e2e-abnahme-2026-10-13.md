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
| Relay on the rented server (212.227.140.164:7777) | Debian 12 VPS | **running** as systemd user service `collab-relay` (build f61c9f1), see "Rented server" |
| 4 editors (1 host + 3 guests) against the **rented** relay | macOS → VPS through an ssh tunnel | PASS, 47 checks |
| Same, **directly** to 212.227.140.164:7777 over the internet | macOS → VPS | PASS, 47 checks (after the port was opened at 15:52) |
| Second platform (Windows/Linux client) | — | **not done**: no client on NN-WS03/NN-RP01 reachable from this step |
| CI (Windows/Linux/macOS build + full ctest) | GitHub | 38396486313: Windows did not compile (MSVC, fixed in 58f1e4f2); 38400975745: Windows builds, 279/280 (timing in one test, fixed in a896172a), others green; 38407041777 (a896172a) still running |

So: unicast and broadcast through a real relay work with several real editor
processes that only dial out, and peer-to-peer works unchanged. The relay
runs permanently on the rented server, and the same four editors share a
session through it, directly over the internet. Not shown: a client on a
second operating system.

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
uses `<cstdint>`). Mac build green.

The next Windows run (38400975745, head 10c700e5) built, and 279 of 280 tests
passed. The one failure was "Relay: a relay nobody runs is a failed connect,
and a host keeps retrying": the host's `lastError()` was still empty. Windows
retries the SYN to a dead loopback port for about two seconds before
`WSAECONNREFUSED`, while Linux and macOS refuse at once, and the test only
pumped for a fixed 200 ms. The code was right (`onLinkClosed` sets the error
once the refusal arrives; the client half of the same test waits for its event
and passed on Windows). Fix `a896172a`: the test waits for the error, up to
10 s. Linux, macOS and lavapipe were green in that run. CI for `a896172a`:
run 38407041777, not yet finished when this report was written.

## Rented server

With the human's go-ahead (request #67), CollabSession `f61c9f1` (the relay
code of `736694a` plus `deploy/`) was built on 212.227.140.164 (Debian 12,
x86_64, GCC 12, 2 cores) with `deploy/deploy.sh horizon@212.227.140.164 7777`
and runs as the systemd user service `collab-relay` (lingering on, so it keeps
running without a login and after a reboot; the account has no passwordless
sudo for a system service). Stop/restart/log commands:
`docs/relay-server-protocol.md`, "Running instance", and the CollabSession
README.

The service listens on `*:7777` and is reachable on the server itself, also
through its public address. **From outside, port 7777 did not answer** at the
time of the run (port 22 did): a firewall in front of the VPS (IONOS cloud
firewall policy, or the host's own) still blocks it. Opening it needs the IONOS
panel or root, so the human was asked (request #70). The host's ufw is enabled
in its config but its service is inactive, and neither nftables nor firewalld
runs, so the IONOS cloud firewall is the likely place. At 15:43, after the
human had added a rule, 7777 still did not answer from the Mac, while the Mac
reached port 7777 elsewhere (portquiz.net) and the relay journal showed no
connection attempt from outside. At 15:51, with the IONOS panel saying the
cloud firewall now lets 7777 through, it was still closed. The host does run a
packet filter after all: nf_tables is loaded with 606 references, together with
xt_recent, xt_LOG, xt_limit, xt_hl, ip6t_rt and ipt_REJECT, the module set of a
loaded ufw ruleset (even though `ufw.service` shows inactive). Reading or
changing it needs the sudo password. To open it, on the server:
`sudo ufw allow 7777/tcp && sudo ufw status` (or the nftables/iptables
equivalent). No reboot is needed for that.

So that the run against the real server did not have to wait for that, the
same E2E ran through an ssh tunnel (`ssh -L 47777:127.0.0.1:7777`), so the
editors' relay address was `127.0.0.1:47777`. What that proves: the deployed
binary on the VPS (Linux, epoll path) pairs, forwards and fans out for four
real editors over the internet. What it does not prove: that the port is open,
and that clients reach the relay without a tunnel. Result: **PASS, 47 checks**
(the four relay-log checks of the local run are replaced by the server's
journal):

```
[accept session=gtbugs.. peer=host -> c-uzuyvcksdg71] paired with #5
[accept session=gtbugs.. peer=host -> c-apalv19qn45v] paired with #6
[accept session=gtbugs.. peer=host -> c-6sx3t0gd0kui] paired with #7
[subscribe … peer=c-uzuyvcksdg71 -> host] subscribed to #4 (1 subscribers)
[subscribe … peer=c-apalv19qn45v -> host] subscribed to #4 (2 subscribers)
[subscribe … peer=c-6sx3t0gd0kui -> host] subscribed to #4 (3 subscribers)
[subscribe … peer=c-uzuyvcksdg71 -> host] disconnected after 7.8 s, received 1748 broadcast bytes
[subscribe … peer=c-apalv19qn45v -> host] disconnected after 7.6 s, received 1748 broadcast bytes
[subscribe … peer=c-6sx3t0gd0kui -> host] disconnected after 7.8 s, received 1835 broadcast bytes
[listen session=gtbugs.. peer=host] disconnected after 11.1 s, forwarded 1739 bytes in 24 broadcasts
```

24 broadcasts left the host once; the guest that stayed longest received all
of them (1739 + 24 × 4 framing = 1835 bytes), and the two that left earlier
missed the last ones, which announced their own departure. Joins 0.1–1.2 s,
syncs 0.0–0.3 s over the internet.

### Direct, without the tunnel (PASS, 47 checks)

At 15:52 port 7777 answered from outside. The same run with
`--relay 212.227.140.164:7777`: every new session link of every editor is a
connection to `212.227.140.164:7777` (host 4, each guest 2: pair and
subscription), and the server journal shows them arriving from the Mac's
public address:

```
[accept session=2fl3k6.. peer=host -> c-h21toqs1q91b] paired with #17
[accept session=2fl3k6.. peer=host -> c-nfy71v5lbmhg] paired with #18
[accept session=2fl3k6.. peer=host -> c-1k5vekjgfxnk] paired with #19
[subscribe … -> host] subscribed to #16 (1 subscribers)   … (2 subscribers)   … (3 subscribers)
[subscribe … peer=c-h21toqs1q91b -> host] disconnected after 5.3 s, received 1484 broadcast bytes
[subscribe … peer=c-nfy71v5lbmhg -> host] disconnected after 5.1 s, received 1484 broadcast bytes
[subscribe … peer=c-1k5vekjgfxnk -> host] disconnected after 5.2 s, received 1571 broadcast bytes
[listen session=2fl3k6.. peer=host] disconnected after 8.3 s, forwarded 1491 bytes in 20 broadcasts
```

Joins 0.2–1.0 s, broadcast and unicast syncs 0.0–0.2 s over the internet.

## Gaps (not shown by this step)

1. **Second operating system.** The editors here all run on macOS. A Windows or
   Linux client would need NN-WS03 or NN-RP01 to run either
   `he_tests --test-case='*real collab_server*'` with
   `HE_TEST_RELAY=<relay>`, or an editor from this script, against the same
   relay. I can't reach those machines from this step. The Windows/Linux
   relay code paths are only covered by CI (in-process FakeRelay, no real
   relay, because CollabSession is a private repo and the public HE CI cannot
   check it out without a token).
2. **Game sessions** (`NetGameSession`) through the relay are covered by the
   real-relay test case (host + join with only the join code), not by
   separate game processes.
3. Guest editors whose world the session replaced do not end on SIGTERM
   (unsaved changes) and get killed after 10 s; their block-buffered logs are
   cut off. The script therefore takes the guest-side witnesses from the
   relay log, not from the guests' logs.
