# Engine-Content-Server in C++: step 3, engine client talks to the new server (2026-11-11)

Topic 194. Steps 1/2 built the standalone server (separate repo, not merged to `main` yet — see
`docs/engine-content-server-cpp-scaffold-2026-11-11.md` and
`docs/engine-content-server-cpp-content-serving-2026-11-11.md`). This step wires the engine's
existing `HE_ContentSync` client to talk to it, configurably, with SFTP staying the default/
fallback.

## Handoff

- **Topic / step:** 194 Engine-Content-Server neu in C++, step 3 of 5
- **Branch / commit:** engine repo `claude/engine-content-server-cpp` (this commit)
- **Done:** `EngineContentSync` (manifest refresh + the download queue) now talks to the new
  HTTP content server when `config.json`'s `"EngineContentServerUrl"` (a `CustomConfig` entry,
  e.g. `"http://127.0.0.1:18080"`) is set, and falls back to SFTP exactly as before when it is
  not — same `refreshManifestWith`/`m_refreshMutex`/rename-guard either way, no change to the
  SFTP code path or to self-update. `EditorApplication.cpp`'s startup probe and
  `ProjectPreflight.cpp`'s dependency check were both updated to treat "content server
  configured" as its own reason to attempt a refresh — previously both gated solely on
  `probeSftp().ready()`, which would never be true for a content-server-only install.
- **Files touched:**
  - `src/HE_ContentSync/include/ContentSync/ContentServerClient.h` /
    `src/HE_ContentSync/src/ContentServerClient.cpp` (new) — `ContentServerEndpoint` (host/port,
    read from config once per process), `contentServerGetManifest`/`contentServerGetFile`
    (both return the existing `SftpResult`, so they plug straight into
    `EngineContentSync::ManifestFetchFn` and the download queue), percent-encoding for
    `GET /content/<path>`, temp-then-rename via the existing `detail::renameReplacing`.
  - `src/HE_ContentSync/src/EngineContentSync.cpp:74-100` (`refreshManifestBlocking`),
    `:289-343` (`drainQueue`) — prefer the content server when configured, construct an
    `SftpConnection` only on the SFTP path.
  - `src/HE_Net/include/Net/HttpClient.h` / `src/HE_Net/src/HttpClient.cpp` — new
    `httpDownloadToFile` (streams a GET body straight to a file instead of buffering the whole
    response in `HttpResponse::body`; verifies the byte count against `Content-Length`, an
    idle-timeout that resets per chunk rather than one fixed deadline for the whole transfer).
    `httpRequest`'s connect+send and status/header parsing were factored into two internal
    helpers (`connectAndSend`, `parseStatusAndHeaders`) so the streamed path reuses them instead
    of duplicating ~70 lines — `httpRequest`'s own behavior is unchanged.
  - `src/HE_Editor/EditorApplication.cpp:2672-2690` (`startSftpProbe`),
    `src/HE_Editor/ProjectPreflight.cpp:109-120` (`run`) — the loosened gate above.
  - `tests/helpers/OneShotHttpServer.h` (new) — a one-shot loopback HTTP server shared by
    `test_net_discovery.cpp` (already had its own copy inline; factored out, no behavior change)
    and the new `test_content_server_client.cpp`.
  - `tests/test_content_server_client.cpp` (new) — URL parsing, the test-only endpoint override,
    manifest/file fetch against the loopback server (including percent-encoding and a
    temp-file/rename check), plus a live case gated on `HE_TEST_CONTENT_SERVER=host:port`
    (same pattern as `test_sftp_connection.cpp`'s `HE_TEST_SFTP`).
  - `tests/test_net_discovery.cpp` — new `httpDownloadToFile` cases (happy path with a 200 KB
    body across several recv chunks, truncated body, missing `Content-Length`, a 404).
- **Verified (NN-WS03, MSVC 19.51, Ninja, Release, build dir `C:/hb/ecs3`):**
  - `cmake --build C:/hb/ecs3 --target he_tests` and `--target HorizonEditor`: both clean, 0
    errors (the only warnings are the pre-existing doctest/`std::tuple` C5285 noise every test
    file already has).
  - Full `ctest -j 8` on that build: **371/371 passing** (the 372nd, `sdk_example_project`, was
    excluded — see Traps).
  - **Live, against the real server binary (step 2's commit 97accd8, built from
    `claude/content-server-step-2-file-serving`)**: started `horizon-content-server.exe` on
    `127.0.0.1:18099` with a one-entry manifest; ran the real `HorizonEditor.exe`
    (`HE_HIDDEN_WINDOW=1`, scratch `APPDATA`/`HE_CONFIG_DIR`, `config.json` with
    `EngineContentServerUrl=http://127.0.0.1:18099`, **no SFTP credentials compiled in**) and
    confirmed in `HorizonEngine.log`: `EngineContent: SFTP endpoint not configured` (expected —
    proves SFTP truly wasn't in play) immediately followed by `EngineContent manifest refreshed
    (1 entries)`, and the exact server JSON landed byte-for-byte in
    `EngineContentCache/manifest.json`. Also ran `test_content_server_client.cpp`'s live case
    (`HE_TEST_CONTENT_SERVER=127.0.0.1:18099`) against the same running server: manifest fetch +
    `GET /content/Materials/DefaultCube.hasset` both succeeded, downloaded size matched.
- **Not verified:** a download triggered through the full editor UI (Content Browser / a scene
  referencing an EngineContent UUID) — the manifest-refresh path above is proven live end to end,
  and the per-file download path (`contentServerGetFile`) is proven live via the test above and
  via unit tests against a fake server that reproduces the real one's exact wire format
  (confirmed against the real server's `FileServer.cpp`/`Http.h` source); wiring an actual
  project + scene reference to exercise `enqueueDownload` through the UI was not done, for time.
  Linux/macOS: unverified (same as steps 1/2 left them — Windows-only device).
- **Decisions taken:**
  - Config key is a single URL string, `"EngineContentServerUrl"`, read through
    `GlobalState::getCustomConfigString` (the same `CustomConfig` mechanism `GameWindowWidth`,
    `RenderPath`, etc. already use) and parsed with the existing `HE::Net::httpParseUrl` — no new
    config file or compile-time plumbing. Empty/missing means "not configured", same semantics as
    `engineContentEndpoint()`'s compiled SFTP host.
  - Unlike `SftpCredentials.h`, this is **not** a compile-time secret: the server has no login,
    so there is nothing here worth keeping out of a tracked file or hiding behind a `-D`.
  - `contentServerEndpoint()` caches like `engineContentEndpoint()` does (a function-local
    static) — one config.json read per process. A `setContentServerEndpointForTesting` escape
    hatch lets tests point at a loopback server without writing a config file or restarting.
  - `httpDownloadToFile` lives in `HE_Net` (not `HE_ContentSync`) because `HE_ContentSync` is
    editor-only (links libssh2, never shipped in the game); a later step giving the packaged
    game HTTP content access can reuse it without pulling in SFTP.
  - Reused `SftpResult`/`SftpProgressFn` as the return/progress types for the new HTTP functions
    instead of inventing parallel ones — they already have exactly the right shape and it means
    `drainQueue`/`refreshManifestWith` needed no signature changes.
  - Publish stays SFTP-only; the new server is read-only (matches step 2's design).
- **Decisions suggested, not taken (queen/human):**
  - Plaintext HTTP: flagged on the topic board (post #2518) — `HE::Net::HttpClient.h` was
    documented LAN-only/no-TLS; this reuses it for a server that (per the topic) will eventually
    sit on a rented, public host. Not a regression (the server itself has no TLS either), but a
    thing to decide before step 4/5 point this at anything but a local test server.
  - `EditorApplication.cpp`'s unreachable/catalogue-unreadable notifications still check only
    `probe.configured`/`probe.reachable` (SFTP-specific) — a content-server-only install that is
    configured but unreachable gets no user-facing notification today (silently falls back to
    the cache, same as "not configured" used to). Fine for this step; a follow-up could extend
    those notifications to name which transport failed.
- **Pending:** step 2's server-repo branch still needs the queen's merge. Step 4 (deploy to the
  rented host) needs to set a real `EngineContentServerUrl` somewhere (packaged config? editor
  default?) — not decided here. The load test (topic Done-when) is not this step's job.
- **Traps:**
  - `ctest`'s `sdk_example_project` test hangs on this device on a from-scratch build dir — a
    nested CMake try-compile fails on a missing `rc.exe` and then appears to sit rather than
    fail fast (confirmed pre-existing: `tests/sdk_example/run_sdk_example.cmake` is already a
    named trap in this topic's Hive lessons). Excluded with `ctest -E "^sdk_example_project$"`
    for this step's full run; not touched, not this step's problem.
  - `std::filesystem::u8path` is deprecated in C++20 and unused anywhere else in this codebase
    (`sftpGetFile` et al. just do `fs::path local = localPath;`) — matched that convention
    instead in the new code, after first writing it with `u8path` and having to walk it back.
  - A multi-line `old_string` with the exact whitespace of an existing block can fail to match
    in the Edit tool for reasons that aren't obvious from the diff (hit this once in
    `EngineContentSync.cpp`'s `drainQueue`) — a single-line anchor edit first, then re-reading
    the file and editing the now-unique placeholder, worked immediately.
  - Running a Windows batch file via `cmd.exe //c <path>.bat` from the Bash tool: a **bare
    relative** filename doesn't resolve even after `cd`; an **absolute path with backslashes
    typed directly into the bash command** gets its backslashes silently stripped (becomes
    `C:ProgramDataHive...`) — use an absolute path with **forward slashes**, or write the
    command into a `.bat` file via the Write tool and invoke that file by its forward-slash
    path. A batch file that itself does `cd <dir>` then invokes a **bare exe name** on the next
    line can fail to find it ("ist entweder falsch geschrieben...") for no obvious reason even
    though `cd` worked — invoking the exe by its **full quoted path** instead fixed it every
    time; never found out why the bare-name form failed.
- **Next steps:** 1. Queen merges the step-2 server-repo branch. 2. Step 4: deploy the server to
  the rented host, decide where a packaged/shipped install's `EngineContentServerUrl` comes from.
  3. Step 5 (or wherever the topic's Done-when load test lives): a real throughput comparison —
  this step only proved correctness, not "measurably faster than SFTP" (not measured here).
