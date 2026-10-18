# Audit-Optimierung: Netz, Tools, Randmodule (Stufe 5)

Thema 219, from `docs/audit/AUDIT.md` Stufe 5 and `docs/audit/modul_net.md`,
`modul_tools.md`, `modul_sourcecontrol.md`, `modul_contentsync.md`, `modul_selfupdate.md`.

## Step 1: NET-101, NET-102/103, NET-201, NET-202/203 (HorizonNet hot paths)

**NET-101 (BitStream).** `writeBits`/`readBits` take as many bits as fit into the open
byte, then whole bytes (at most five rounds for 32 bits) instead of one round per bit.
`writeBytes` appends verbatim when byte-aligned and otherwise shifts each byte across the
open byte; `readBytes` is a `memcpy` when aligned. A short `readBytes` still reads the
whole bytes that are there, moves past them and returns false, like the byte loop did.
New helpers: `byteSize()`, `reserveBytes()`, `appendTo()`. The wire format is unchanged.

**NET-102 (NetSession).** `frame()` is now the one place a frame is built:
`[id low][id high][payload]`, appended directly. Before, there were three vector copies
and a bit pass per `send`. `broadcast`/`broadcastExcept`/`sendTo` build the frame once
for all peers. The peer-list snapshot in `broadcast()` stays (the audit wanted it gone;
the gain is one small vector copy per broadcast, and the advice was not to change it
without proof that `send` cannot re-enter).

**NET-103 (CollabSession).** `sendAssetChunks(to, from, a)` takes the target list and
sends each frame through `NetSession::sendTo`. Begin, every chunk and End are serialized
once, no longer once per peer. Over a relay the fan-out (one frame, one encryption) now
applies to assets. Per peer the order is still Begin → chunks → End; only the streams of
different peers interleave. Callers: host publish (all peers), client → host (one),
`sendAssetTo` (one), and the host relaying a client asset (all except the sender).

**NET-202/203 (Tcp/Secure).** The TCP send queue has a read head (`Conn::outHead`) and no
longer calls `erase(begin)` per flush. It is cleared when drained and compacted only when
more than half of it is sent, so each byte moves at most once on average. The exact
`reserve` in `send()` is gone, and with it the full copy per appended frame.
`SecureTransport::m_events` is a `std::deque` with `pop_front` (before: O(n²) per drain).
`inBuf` was left alone: its `erase` runs once per tick and only moves the partial frame
behind the last complete one.

**NET-201 (UDP).** New POD `SocketAddress {family, ip[16], port, scope}` in `Socket.h`,
without platform headers. Also new: `socketParseAddress`, `socketAddressText`, and
`socketSendTo`/`socketRecvFrom` overloads. The string versions run through the same
internal sendto/recvfrom, so LanBeacon, PortMapper and the tests are unchanged.
`UdpTransport` parses once (connect and accept) and keys `m_byAddress` by address with
`samePeer` equality: family + address + port, scope ignored, v4-mapped ≠ v4. That is
exactly the equivalence the old `stripScope(host)|port` strings had. The cookie HMAC runs
over the address bytes, and only the host that issued it checks it. `Peer::host` stays as
text for the logs; the log macros evaluate their arguments lazily.

### Measurements (standalone `clang++ -O2`, M5, old bit loop vs new, same machine run)

| Case | old | new | factor |
|---|---|---|---|
| 64 MiB `writeBytes`, aligned (incl. `data()`) | 1432–2417 ms | 45–91 ms | ×27–32 |
| 64 MiB `readBytes`, aligned | 1247–1992 ms | 9–27 ms | ×72–145 |
| 64 MiB `writeBytes`, unaligned (after resize fix) | 1368 ms | 172 ms | ×8 |
| 64 MiB `readBytes`, unaligned | 1217–2329 ms | 11–35 ms | ×56–107 |
| 256 KiB asset chunk to 7 peers (102+103) | 75–134 ms | <0.15 ms | ×900+ |
| ~40-byte message framed for 8 peers | 12.7–23.3 µs | 0.5–1.1 µs | ×21–27 |

The spread comes from machine load: other workers were building at the same time, and
every run measured old and new back to back. The benchmark source is not in the repository
(it was `/tmp/he219_bench/bench.cpp`). It copies the old classes verbatim, the same way the
reference test does. TCP/UDP were not timed: no load test exists, and loopback says little
about real networks.

## Handoff (step 1)

- **Topic / step:** 219, step 1 of 3
- **Branch / commit:** `claude/audit-optimierung-netz-tools-randmodule-stufe-5`, commits
  e67de8d8 (101), 0f71214f (102/103), 7028a401 (202/203), 489c16c2 (201), plus this doc;
  pushed.
- **Files touched:**
  - `include/Net/BitStream.h`: `writeBits` :31, `writeBytes` :81, `appendTo` :125,
    `readBits` :150, `readBytes` :208.
  - `NetSession.cpp`: `broadcast` :34, `frame` :77.
  - `CollabSession.cpp`: `sendAssetChunks` :1711.
  - `TcpTransport.cpp`: `flushOutput` :282.
  - `Socket.h`: `SocketAddress` :298; `Socket.cpp`: `socketParseAddress` :809,
    `sendToSockaddr` :1177.
  - `UdpTransport.cpp`: `makeCookie` :251, `findPeer` :473.
- **Verified** (macOS, Release, `out/build/release`):
  - All 15 `test_net_*` files are green, each run with `he_tests --source-file`: bitstream
    10/10 (44k assertions), tcp 13/13, udp 21/21, collab 115/115, relay 27/27, secure,
    lossy, discovery, directory, rpc, property_replication, game_session, team, lanbeacon.
  - New tests:
    - BitStream: the old bit loops are kept as the reference and compared byte for byte
      over 400 random sequences; short reads; a hand-pinned unaligned golden frame.
    - Frame bytes for 0x1234 + 3-bit payload = `34 12 05`.
    - A host on a fanning transport sends each asset frame exactly once to {1,2,3}.
    - 64 × 1 MiB queued at a peer that is not reading yet.
    - `SocketAddress` identity (v4, v6, v4-mapped, scope, hash) and a send/receive round
      trip.
  - Mutation check of the BitStream tests (standalone, header copy): 4 real mutants
    (unaligned read shift, write carry, short read rounding up, high bit leaking in
    `writeBits`) are all caught. One mutant was equivalent (`& 0x7F` on a ≤7-bit carry).
  - Full `he_tests` (without `test_material_graph`): 5336 cases, 3 failed, none in
    HE_Net. The failures and why they are not this step:
    - `test_hpak` scanContentDirectory (6 ≠ 2): passes with an empty HOME (59/59), as
      d83c87f2 describes.
    - Two `EngineProfiler` cases: order-dependent, as the 213 handoff describes;
      `test_profiler.cpp` alone passes 18/18.
- **Not verified:**
  - Windows/Linux only through CI run 39105885328.
  - Link-local IPv6 (sending with the scope from recvfrom) has no test; it needs a real
    interface.
  - No real collab session with several editors.
- **Decisions taken:** keep the `broadcast()` snapshot copy; leave `inBuf` alone; the
  cookie input changes, which is safe because cookies never leave the host that issued
  them.
- **Pending (topic):** step 2 (TLS-101/102/201/203), step 3 (SCM/CSY/SUP).
- **Traps:**
  - `he_tests` lives at `out/build/release/tests/he_tests`; run it from the build
    directory.
  - A fresh worktree build fills from the shared ccache; configure with the main
    checkout's `_deps` (see `CLAUDE.md` and the memory note on worktree configure).
  - Do not touch `BitStream.h` while a build is running: about 25 TUs include it.
- **Next steps:**
  1. If CI is red on Windows/Linux, start in `Socket.cpp` `toSockaddr`/`fromSockaddr`
     (socklen_t, sin6_scope_id).
  2. Step 2 (Tools) starts at `docs/audit/modul_tools.md` TLS-101.

## Step 2: TLS-101/102, TLS-201, TLS-203 (offline tools)

**TLS-101.** `Importer::writeAsset` built a full `ContentManager` per asset to call
`saveAsset`. New `HE::saveAssetFile(asset, contentRoot)` (`ContentManager/AssetFile.h`,
defined next to `writeAssetFile` in `ContentManager.cpp`) writes the same file: same
export gate, UUID minting, folder creation, log line. `saveAsset` shares its two halves.
`setTextureSrgb` still builds a manager (it has to `loadAsset`; one per dialog row).
**TLS-102.** `meshSidecarAssets` streams MREF and MTRL with `readOneChunk` instead of
`HAsset::Reader::open` (which read every vertex buffer). Pre-v2 files now give an empty
list (no test fixture is pre-v2). **TLS-201.** `ensureGlslangProcess()` in
`ShaderCompiler.cpp`: init once, finalize at exit; the mutex stays. **TLS-203.** The
manifest code moved unchanged from `ProjectExporter.cpp` into `Hpak/PackManifest.{h,cpp}`
(`packSettingsFingerprint`, `openIncrementalCache`, `writeIncrementalManifest`); the
export uses it, `hpak_packer` has `--cache <manifest>`.

Measured (Release, M-series, load 4-7, no `yes` hogs, low-power off):

| | before | after |
|---|---|---|
| `he::shaderc::compile`, small fragment shader, mean of 100, 3 alternating runs | 183-219 ms | 3.3-3.5 ms |
| one asset write (256×256 RGBA texture), mean of 50 | 1.82 ms | 0.85-0.95 ms |
| `hpak_packer` 40 EngineContent assets, zstd, second run | 0.47-0.93 s | 0.09 s (40/40 reused) |

## Handoff (step 2)

- **Commits:** 53a2234e (TLS-201), 0fa1921e (TLS-203), 5b04a439 (TLS-101/102 + tests),
  plus this doc. Stopped on the queen's request after everything was verified.
- **Verified:** build of `he_tests`, `hpak_packer`, `asset_compiler` green. New
  `test_tools_writer_cache.cpp` 4/4 (saveAssetFile bytes == saveAsset bytes, UUID, gate;
  manifest reuse byte for byte, rejected for other codec/key/cook, stale manifest,
  missing files). Importer, ContentManager, export, hpak, sky/engine-material tests green.
  Full `he_tests` without `test_material_graph`: 5340 cases, 3 failed: the same three as
  step 1 (`test_hpak` scanContentDirectory passes with empty HOME, 2× EngineProfiler).
  The bench binary exits 0 after the static glslang finalize. CLI: `--cache` pak is
  byte-identical to a pack without cache; `--secret` reuses 40/40, other key falls back.
- **Not verified:** `test_material_graph` (the case at :4789 brackets glslang itself;
  refcounting makes that safe, but it was not run). Windows/Linux only via CI.
  `asset_compiler` end to end was not A/B'd (bytes equal by construction + test).
- **Traps:** benches are in `/tmp/tls201`, `/tmp/tls101` (link `libhe_shadercompiler.a`
  statically, so no dylib A/B trap). `compile_commands.json` in the worktree is stale; take
  INCLUDES from `build.ninja`.
- **Next steps:** step 3 (SCM/CSY/SUP). Optional: `landscape_tex_gen`/`MatGen` could use
  `saveAssetFile` too (TLS-117, not in scope).

## Step 3: SCM-101/102/103, SUP-101, CSY-101 (source control, self-update, SFTP)

**SCM-101 (GitService refresh).** 8 git processes per refresh became 4 when nothing is
ahead: `GitCli::listLocalAndRemoteBranches` (one `for-each-ref --format=%(refname)
refs/heads refs/remotes`, split on the prefix, same remote filters; `switchBranch` uses it
too), no discarded `branch --show-current`, `log(..., checkUnpushed)` runs the `rev-list`
only when status reports an upstream with `ahead > 0`, origin's URL cached worker-side
(re-read after `Open` and every user command, and once it is 60 s old). Not done:
`status --show-stash` (needs git ≥ 2.35, absence is ambiguous). Only difference to
`%(refname:short)`: a local branch whose name also exists as a tag now lists as `x`, not
`heads/x` (which `git switch` takes either way).
**SCM-102 (branch dialog).** `GitService::requestCheckBranchName` / `branchNameCheck(name)`:
the worker runs the same `check-ref-format` + `rev-parse` as before; the panel asks once
per change of the text, shows a verdict only for the current text, Create waits for it.
Queued checks collapse, the answer is not "work" and leaves `lastError`/`lastInfo` alone.
**SCM-103 (TeamSync `scan()`).** `lexically_relative` against the root the walk starts at
(trailing separator dropped), `fs::relative` only as fallback for a `..`/empty result.
**SUP-101 (self-update takeover).** Option B of the audit: `Session::install()` runs
`beginTakeover` through `runWorker` (returns `bool started`), new phase `Installed`;
`SelfUpdateDialog::finishInstall()` is polled next to `Draw` (`EditorUI.cpp`) and quits on
`Installed`, reopens the dialog on failure. The modal shows "Preparing version ..." without
Close and comes back if dismissed. Option A (unpack in the download worker) not done.
**CSY-101 (SFTP).** `HE::Cs::SftpConnection` (pimpl, `SftpClient.h`): one session across
operations; the free functions are one-shot wrappers. Publish, rebuild and `drainQueue`
hold one per run; publish ensures each remote directory once. A connection fault (libssh2
errno other than `LIBSSH2_ERROR_SFTP_PROTOCOL`) drops the session and, if it had carried
an earlier operation, retries once on a fresh one. A listing cut short by a connection
fault now fails instead of returning a smaller tree.

Measured (Release, M-series, load ~2):

| | before | after |
|---|---|---|
| git refresh, 200-file repo with upstream, mean of 20, 3 rounds | 278-282 ms (8 procs) | 145-148 ms (4) |
| git refresh, this repository, mean of 5, 3 rounds | 403-420 ms | 232-249 ms |
| branch dialog open, per frame on the UI thread | 2 git procs (~70 ms) | 0 |
| `scan()` walk, 20,000 files depth 4 under `/tmp` (symlinked), 5 rounds | 2.34-2.44 s | 0.52-0.59 s |
| 20 SFTP downloads, local paramiko server, loopback | 4.34 s (20 sessions) | 0.60 s (1 session) |

The SFTP numbers are a loopback Python server (~200 ms per handshake, mostly key
exchange in Python); the hoster's real handshake was not measured (no endpoint is
configured in this build, and the shared library must not be written to from a test).

## Handoff (step 3)

- **Commits:** bf10eb8a (SCM-101/102), 3e7c6864 (SCM-103), 4083435b (SUP-101),
  db20ee95 (CSY-101), plus this doc.
- **Verified:** `he_tests` and `HorizonEditor` build green; `editor_help_audit --check` ok.
  `test_git_roundtrip` 42/42 (new: refresh lists, unpushed only when ahead, URL cache
  re-read after a user command, name check answers per text, does not clear lastError,
  burst collapses), `test_git_panel_ops`, `test_git_status_parse`, `test_team_sync` 12/12
  (new: root via symlink and with trailing slash), `test_editor_selfupdate` 7/7 (new: begin
  runs on the worker, Installing while held, no second install), new `test_sftp_connection`
  4/4 (no endpoint; missing local file = no handshake; dropped handshake = one transient
  failure, no retry). Live: `HE_TEST_SFTP=127.0.0.1:2222:tester:pw123:/content` against
  `/tmp/he_sftp_scripts/sftp_server.py` (paramiko 5.1 in `/tmp/he_sftpsrv/venv`): 127
  assertions, 22 sessions for 1 batch + 20 one-shot + 1 connection; server restarted
  mid-connection → one reconnect, success (`/tmp/he_csy101/reconnect.cpp`). Full
  `he_tests` (private HOME/TMPDIR, without `test_material_graph`): 5347 cases, 2 failed,
  the known order-dependent EngineProfiler pair (`test_profiler.cpp:107`, `:215`).
- **Not verified:** the editor's update dialog by hand (no GUI driving; the e2e script
  `selfupdate_editor_e2e.py` was not run, it needs a staged server). Windows/Linux only via
  CI. Real hoster timing.
- **Traps:** the paramiko server must keep the channel from `Transport.accept()` or the
  client hangs in `libssh2_sftp_init` forever: CSY-102 (no libssh2 timeout) is real and
  still open. Benches: `/tmp/he_scm103/bench.cpp`, `/tmp/he_scm101/bench.sh`.
- **Next steps:** CSY-102 (`libssh2_session_set_timeout`), SUP-101 option A (unpack in the
  download worker), SCM-104 (one scan per sync cycle). Stufe 5 is done after CI.
