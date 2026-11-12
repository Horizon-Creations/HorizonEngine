# Engine-Content-Server in C++: step 4, deploy to the rented host + performance audit (2026-11-12)

Topic 194. The server runs on the IONOS host as a systemd user service; this doc records what
was measured against it and what is left. Server repo work is on branch
`claude/content-server-step-4-deploy` (on top of step 2's `claude/content-server-step-2-file-serving`
@ 97accd8; neither is merged, no PR exists as of 2026-11-12).

## Handoff

- **Topic / step:** 194, step 4 of 5. Engine branch `claude/engine-content-server-cpp`.
- **Server, live:** `212.227.140.164` (Debian 12, 2 cores, 3.8 GB RAM, no sudo, linger already on),
  user service `horizon-content-server` (enabled, `Restart=always`), port **7780**, bound to
  **127.0.0.1** only. Config `~/content-server/server.conf`, binary `~/content-server/bin/`,
  content `~/content-server/content/<UTC stamp>/` with `content/current` as symlink. collab-relay
  (7777) untouched. Verified on the host: `systemctl --user restart` -> active again,
  `ss -ltn` shows 127.0.0.1:7780, `curl localhost:7780/manifest.json` returns 107 entries,
  `/content/Meshes/Capsule.hasset` 200 / 32251 bytes.
- **Content on the host:** 107 files, 740 MB, copied from the real EngineContent: the 4.3 MB
  EditorDeps set (many small `.hasset`) plus 9 large real files (2x16.8 MB textures, 6 wavs 52-96 MB,
  one 290.8 MB landscape array). The 1 GiB landscape arrays were left out (upload time). The server
  scans the directory (no manifest.json in the set).
- **Server repo, new files:** `deploy/deploy.sh` (git archive over ssh, build on host, unit,
  restart, listening check), `deploy/publish-content.sh` (tar stream over ssh, new release dir,
  atomic symlink swap, restart, keeps 3 releases), `deploy/horizon-content-server.service`,
  README "Deploy" section. Host has **no rsync**, so publish is a full tar upload.
- **Replace-while-downloading on Linux (verified):** scratch server on the host, 96 MB file
  downloaded at 15 MB/s, file replaced via `mv` over the path mid-transfer: transfer stayed in
  flight and finished byte-exact vs the ORIGINAL. publish-content.sh never replaces in place
  anyway (option 1 of step 2's doc); the restart after the swap does cut transfers in flight.
  Byte-exactness through the tunnel also checked (sha256 of Rain1.wav identical to source).
- **Firewall: OPEN POINT FOR THE QUEEN.** Port 7780 is not opened (no ufw / IONOS panel change).
  To go live: open TCP 7780 in the IONOS cloud firewall (and ufw if active), then set
  `host = 0.0.0.0` in `~/content-server/server.conf` and `systemctl --user restart
  horizon-content-server`. Until then the server is reachable only by `ssh -L 7780:127.0.0.1:7780`.
  Clients need `EngineContentServerUrl` = `http://212.227.140.164:7780` in config.json; **no
  default is compiled in** (step 3's decision), so a shipped install still needs a place to set it.
  Plain HTTP, no TLS (flagged in step 3, not decided).
- **Editor against the real server (verified):** release build of this branch
  (`out/build/release`, built without SFTP credentials), `HE_CONFIG_DIR=<scratch>` with
  `EngineContentServerUrl=http://127.0.0.1:17780` (ssh tunnel to the VPS), `HE_HIDDEN_WINDOW=1`,
  300 frames: log shows `SFTP endpoint not configured` then `EngineContent manifest refreshed
  (107 entries)`; 107 = the VPS manifest, not the 99 of the production SFTP. `he_tests` with
  `HE_TEST_CONTENT_SERVER=127.0.0.1:17780` (live manifest + file download case): 7/7 pass.
  Net/ContentSync/sftp test files (550 cases) pass after the fix below.
- **Packed game: NOT done, not possible today.** `HorizonGame` (`src/HE_Game`) has no content-server
  or ContentSync client at all (HE_ContentSync is editor-only, step 3 said so); it reads its pak.
  The topic's "packed game loads content from the new server" needs a game-side client first
  (`httpDownloadToFile` lives in HE_Net, so it is reachable). That is a missing step, not a deploy item.
- **Engine-side diff (one fix, found by the audit):** `HE::Net::httpRequest/httpDownloadToFile`
  slept 2 ms in the connect loop (`HttpClient.cpp`, `connectAndSend`). macOS stretches that to
  ~120 ms, so every request cost >=120 ms locally and ~1.5 s through the tunnel regardless of file
  size (a 262-file sync would be minutes of pure waiting). New `socketWaitConnected`
  (`Socket.h/.cpp`, select on write+except) replaces the sleep. Measured with the engine's own
  client (standalone harness built from these sources): small file, local loopback server,
  **120-130 ms -> 0.5-1.4 ms**; manifest 0.3 ms; through the tunnel 1.5 s -> 52 ms (manifest) /
  67-158 ms (32 KB file). The 2 ms sleep in the send loop and on the SFTP side were not touched.
  Windows' 15 ms timer tick makes the same sleep expensive there; not measured on Windows.
- **Server code: no change.** Profile result below: the server is nowhere near the bottleneck, so
  by the step's rule nothing was fixed there.
- **Server repo tests on macOS (not fixed, pending):** step 2's tests were green on Windows only.
  On macOS `hcs_tests` has 3 failures (test temp dir is behind the `/var` -> `/private/var`
  symlink, serveFile's containment check expects a canonical dir; production is fine because
  `Listener::setContent` canonicalizes) and, once that is fixed, "serveFile's stop flag aborts a
  stalled send" hangs: `sendAll` polls writable then does a blocking `send` of 64 KB, which on BSD
  blocks when only `SO_SNDLOWAT` bytes are free. `MSG_DONTWAIT` alone did not cure it on macOS
  (still blocked in `__sendto`), so I reverted both and left the code as step 2. On the **Linux
  host the behaviour is fine**: a client that never reads, then SIGTERM -> server exited in 104 ms.
- **Side effect to know about:** the editor run rewrote the real
  `~/Library/Application Support/HorizonEngine/EngineContentCache/manifest.json` (the cache dir is
  not redirected by `HE_CONFIG_DIR`) with the VPS's 107-entry manifest. I restored it by fetching
  the production manifest again with the engine's SFTP client: 19515 bytes / 99 entries, same size
  as before. No cached content file was touched.
- **Not measured:** Windows client numbers; HTTP over a direct (non-tunnelled) connection
  (port closed); SFTP numbers for the production webspace at other times of day; more than one
  concurrent client over the network; the 1 GiB files; editor-driven file download through the UI.
- **Next steps:** 1. Queen: open port 7780, flip `host`, decide the URL default / TLS.
  2. Queen: merge the server branches (step 2 + step 4 are stacked). 3. Game-side HTTP content
  client (above). 4. Optional: fix the macOS test issues in the server repo.

## Performance audit

Method. Client: this MacBook Air (home internet), idle. Host: the VPS. Same files, runs
interleaved per file (HTTP, SFTP-VPS, SFTP-prod, repeat) so link drift hits all three alike.
Seconds are wall time per whole file (incl. connect/handshake/auth). The port is closed, so:

* **HTTP** = `curl` (and the engine's own client, see below) through `ssh -L` to the server.
* **SFTP-VPS** = OpenSSH `sftp` to the same VPS, same files. Both go through one ssh session, so
  cipher cost is equal; the difference is the protocol/handshake.
* **SFTP-prod** = the engine's own libssh2 `sftpGetFile` against the production SFTP webspace,
  the "bisherige Loesung" as the editor really uses it (compiled-in endpoint, credentials never
  read). A different machine and location than the VPS.

Latency, 32 KB file (`Meshes/Capsule.hasset`), 10 interleaved runs:

| path | median | range |
|---|---|---|
| HTTP via tunnel, curl, whole file | **76 ms** | 73-91 ms |
| HTTP via tunnel, curl, time to first byte (earlier set) | 47 ms | 46-105 ms |
| HTTP via tunnel, engine client (fixed), whole file | 69 ms | 67-158 ms |
| HTTP manifest (13.7 KB), engine client (fixed) | 51 ms | 48-103 ms |
| SFTP, OpenSSH, VPS | 964 ms | 916-1022 ms |
| SFTP, engine libssh2, production | 775 ms | 698-910 ms |

A fresh SFTP session per file costs ~0.8-1 s (connect + key exchange + auth); HTTP costs one RTT
(~45 ms to the VPS). That is a ~10-13x latency gain per small file; the editor fetches content
file by file, so this is the main win for many-small-file syncs. (The engine-client number holds
only with the connect-loop fix above; before it, 1.5 s through the tunnel.)

Throughput, interleaved, median (best run), MB/s:

| file | HTTP tunnel (curl) | SFTP VPS | SFTP prod (engine) | runs |
|---|---|---|---|---|
| 16.8 MB texture | 2.45 (2.60) | 2.06 (2.71) | 1.94 (2.13) | 5 |
| 51.9 MB wav | 3.71 (13.7) | 4.37 (12.6) | 3.81 (4.99) | 4 |
| 290.8 MB texture array | 11.0 (11.4) | 10.5 (11.9) | 4.23 (4.31) | 2 |

Honest reading: over this home link the transfer is **link-bound and noisy** (the same HTTP
download ranged 3.8-21.8 s for 52 MB within one hour; the link ran 2.5 MB/s early and 15 MB/s
later). HTTP-through-ssh and SFTP-VPS are the same within that noise, which is expected since both
ride one ssh session. The one clean throughput difference is against the production webspace:
**290 MB: 4.2 MB/s (engine SFTP) vs 11.0 MB/s (HTTP/VPS), 2.6x**, but that mixes the protocol
with a different host and route, so it is not a pure server comparison. A direct, untunnelled HTTP
number needs the port open: **not measured**. The engine client alone (fixed), via tunnel: 16.8 MB
in 1.08 s (15.5 MB/s) twice and 3.96 s once; 96 MB in 5.6 / 8.3 / 9.7 s (10-17 MB/s).

Server profile (on the host, loopback, `curl` to `127.0.0.1:7780`, isolates the server from the
link; 2 cores, file in page cache):

| load | result |
|---|---|
| 32 KB file | 0.3-0.4 ms total, TTFB 0.25-0.37 ms (n=10) |
| 16.8 MB | 5.0-6.4 ms (n=5) |
| 52 MB | 13.6-17.2 ms (n=5) |
| 96 MB | 29.7-33.6 ms (n=3) |
| 290.8 MB | 91.2-91.5 ms = 3.2 GB/s (n=3) |
| 1 / 4 / 8 / 16 parallel x 96 MB | aggregate 2.4 / 3.5 / 2.9 / 3.1 GB/s, host load stays ~0 |

The server serves 2-3 GB/s on 2 cores with 16 concurrent clients, orders of magnitude above any
realistic internet link, so **no server bottleneck exists to fix**; the 64 KB read+send loop is
adequate. The measurable bottlenecks were the client's connect-loop sleep (fixed) and the SFTP
per-file session setup (the reason for the whole topic). The real ceiling for users is the
host's uplink (not measured) and their own link.
