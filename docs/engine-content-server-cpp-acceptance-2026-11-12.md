# Engine-Content-Server in C++: step 5, build + test acceptance of both repos (2026-11-12)

Topic 194. Foreground build/test of the engine branch (HEAD f278c8536) and the server repo's two
branches. No engine source changed in this step.

## Handoff

- **Engine build (measured):** `cmake --build out/build/release --target HorizonEditor he_tests -j8`
  -> exit 0 (after `cmake out/build/release/sdk-release` to clear the stale-configure "dev MISMATCH").
  172 warnings, none in `HE_ContentSync`/`HE_Net`/`EditorApplication`/`ProjectPreflight`: vendored
  `entt/core/hashed_string.hpp` (-Wdeprecated-literal-operator), `InspectorPanel.cpp:3383`
  (null char in literal), `EditorUI.cpp:460` (enum-enum conversion), two `ld: duplicate libraries`.
  All pre-existing, not touched.
- **Engine tests (measured):** `test_content_server_client` 7/7 (33 assertions; 39 with
  `HE_TEST_CONTENT_SERVER=127.0.0.1:17780`, i.e. live manifest + file download against the VPS),
  `test_engine_content_manifest_lock` 2/2, `test_engine_content_manifest_refresh` 4/4 (0 assertions,
  those cases only drive the SFTP-gated paths). Full `he_tests` with `HE_CONFIG_DIR=<scratch>`:
  `6218 test cases | 6217 passed | 1 failed | 20 skipped`, `2380168 assertions | 2 failed`.
  The one failure is `test_shortcuts_ui.cpp:218/255` ("shortcuts page ... rebinds and persists",
  `h.persisted()` empty): it fails only with `HE_CONFIG_DIR` set (known, see lesson "HE_CONFIG_DIR
  schlaegt den Pin") and passes (2/2) without it. Unrelated to this branch.
- **Server repo (measured, macOS):** both branches build with no warnings. Step 2 branch
  (97accd8): 3 failures (`/var` symlink). Step 4 branch: **fixed the symlink** in
  `tests/test_fileserver.cpp` (`makeTempDir` returns `std::filesystem::canonical(dir)`, production
  `Listener::setContent` already canonicalizes), commit 6f69c12, pushed to
  `claude/content-server-step-4-deploy` (not main). Result: 30/30 pass with the stalled tests
  excluded. The fix lives only on step 4 (stacked on step 2), so step 2 alone stays red on macOS.
- **Still failing on macOS, documented, not fixed:** `serveFile's stop flag aborts a stalled send
  instead of hanging` (`tests/test_fileserver.cpp`) hangs forever (3/3 alone, 5/5 in the full
  suite): `sample` shows the worker blocked in `__sendto` <- `hcs::http::sendAll` <-
  `hcs::serveFile`. `sendAll` polls writable then does a blocking `send` of up to 64 KB; BSD wakes
  select at `SO_SNDLOWAT` free bytes and the blocking send then waits for all 64 KB. The listener
  stall tests (`stop() still returns run() promptly while a large download is stalled`, `a slow
  download does not block`) pass 3/3. Linux is fine (step 4 verified on the host). Not touched per
  the step's "no send logic change"; a real fix is a non-blocking socket (O_NONBLOCK + EAGAIN
  re-poll) in `sendAll`, `MSG_DONTWAIT` alone did not help in step 4's attempt (not re-tried).
- **End-to-end editor (measured):** `HE_CONFIG_DIR=<scratch>` with `CustomConfig`
  `EngineContentServerUrl=http://127.0.0.1:17780` (ssh -L to the VPS), `HE_HIDDEN_WINDOW=1`,
  `HE_EXIT_AFTER_FRAMES=300`: log `EngineContent: SFTP endpoint not configured` (WARN, build has no
  credentials) then `EngineContent manifest refreshed (107 entries)` (VPS manifest; production
  SFTP has 99). Package download: not clicked through the UI; proven by the live test case
  (manifest non-empty, mesh bytes equal to the served file). Config format trap: the key goes into
  `"CustomConfig":[{"Key":..,"Value":..}]`; a top-level key is ignored and the file overwritten.
- **Cache safety:** the editor run rewrote the real `EngineContentCache/manifest.json` (20109 bytes);
  restored from the pre-run backup, `cmp` identical afterwards (19515 bytes / 99 entries). Real
  `config.json` `cmp` identical to its backup after all runs. Tunnel and no editor process left.
- **Step-4 performance numbers:** taken from `engine-content-server-cpp-deploy-audit-2026-11-12.md`,
  not re-measured here (manifest 0.3 ms server side, tunnel 1.5 s -> 52 ms per request after the
  connect fix, SFTP ~0.8-1 s vs HTTP ~70 ms per small file, 290 MB 4.2 vs 11.0 MB/s). This step
  only re-fetched the manifest once (0.11 s through the tunnel incl. ssh hop, curl, n=1).
- **Not verified:** Windows server build (NN-WS03 offline); packed game (by decision, game has no
  content client); direct non-tunnelled access (port 7780 closed, host untouched); Linux server
  tests; CI.
- **Next steps:** 1. Human: open TCP 7780, flip `host`, decide URL default/TLS. 2. Queen: merge
  server step 2 + 4 (squash together, the test fix is on step 4). 3. Optional: non-blocking
  `sendAll` so the stalled-send test passes on macOS.
