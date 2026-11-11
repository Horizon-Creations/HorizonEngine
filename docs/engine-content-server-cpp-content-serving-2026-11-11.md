# Engine-Content-Server in C++: step 2, manifest + file serving (2026-11-11)

Topic 194. Still nothing of the new server lives in this repo (see step 1's doc,
`docs/engine-content-server-cpp-scaffold-2026-11-11.md`); this doc only records where it is and
what is open. All work for this step is in the separate repo, on a branch, not yet merged.

## Handoff

- **Topic / step:** 194 Engine-Content-Server neu in C++, step 2 of 5
- **Branch / commit:** engine repo `claude/engine-content-server-cpp` (this doc only, no code
  change here). Server repo `https://github.com/Horizon-Creations/horizon-content-server`,
  branch `claude/content-server-step-2-file-serving` @ 97accd8, pushed, **not merged to `main`**
  (step 1 had explicit queen approval to push to `main` directly; this step did not, so it sits
  on its own branch for the queen to merge — PR link in the push output:
  `https://github.com/Horizon-Creations/horizon-content-server/pull/new/claude/content-server-step-2-file-serving`).
- **Done:** the server answers `GET /manifest.json` (loads `content_dir/manifest.json` if
  present, otherwise scans `content_dir` recursively and builds one in memory) and
  `GET /content/<relative path>` (streamed, `Content-Length`, one `file_size()` + one open/read/
  close per request). Accepted connections go through a small bounded worker pool instead of the
  scaffold's inline per-connection loop, so one slow/rate-limited client cannot block another
  connection or a shutdown in progress.
- **Files touched (server repo):** `src/Manifest.h/.cpp` (new — parse/write/scan,
  FNV-1a hashing), `src/FileServer.h/.cpp` (new — path resolution/containment, streaming),
  `src/Http.h/.cpp` (new — response building + a stop-aware send loop, shared by Listener and
  FileServer so the two don't duplicate it), `src/Listener.h/.cpp` (routing, nested
  `Listener::Pool` worker pool replaces the scaffold's inline serve loop), `src/Net.h/.cpp`
  (added `waitWritable`, refactored the poll wrapper to share it with `waitReadable`),
  `src/main.cpp` (loads the manifest, wires it into `Listener::setContent`), `CMakeLists.txt`
  (new sources/tests), `README.md` (status, layout, decisions), `tests/test_manifest.cpp` (new),
  `tests/test_fileserver.cpp` (new), `tests/test_listener.cpp` (scaffold's "any path returns 200"
  assumption no longer holds under real routing — rewritten, plus new routing/concurrency/
  shutdown-under-load cases).
- **Schema decision (read before touching the client in step 3):** the manifest JSON here is
  **not** the field names the assignment described (`relativePath`/`uuid` string/`contentHash`).
  It matches the engine's own `HE::Cs::EngineContentManifestEntry`
  (`src/HE_ContentSync/include/ContentSync/EngineContentManifest.h` and
  `EngineContentManifest.cpp` in this repo) field-for-field instead: keys `path`, `uuid` (object
  `{"hi", "lo"}`, absent/zero for a raw file), `hash`, `size`, `mtime` — same FNV-1a constants as
  `Hpak::fnv1a64Bytes` (`src/HE_Core/include/Hpak/HpakFormat.h:126`). Deliberate: matching the
  client's existing parser means step 3 needs no translation layer. `mtime` is 0 for a `.hasset`
  entry (change-tracked by hash) and set for a raw/loose entry (change-tracked by mtime), exactly
  like the engine's own publish side already does it.
- **Verified (NN-WS03, MSVC 19.44.35230, Ninja, Release, this device):** clean
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j 8`: 0
  warnings on a from-scratch rebuild (grepped the full build log). `ctest`: 100% (1 binary,
  `hcs_tests`, 32 test cases / ~292 assertions, ~39 s). `clang-format --dry-run --Werror` clean
  on every new/modified file (VS's own LLVM 19.1.5 copy) after one `-i` pass. Manual run of the
  real `horizon-content-server.exe`: `curl http://127.0.0.1:18080/manifest.json` returned valid
  JSON with the expected entries; `curl .../content/Materials/DefaultCube.hasset` downloaded a
  file whose sha256 matched the source exactly. Concurrency: a 5 MB file downloaded with
  `curl --limit-rate 500k` (~9.5 s) ran at the same time as a `GET /manifest.json` that completed
  in milliseconds — confirmed still in flight via a PID check, not just by timing.
- **Not verified:** SIGINT against the compiled binary while a transfer is in flight. Attempted
  via a Win32 `AttachConsole`/`GenerateConsoleCtrlEvent(CTRL_C_EVENT)` harness (the
  known-documented way around git-bash's `kill -INT` not reaching a native exe, see the Hive
  lesson on this device) but the event did not reach the server process in this sandboxed
  session (no observable effect; the in-flight `curl` ran to completion). Did not retry a third
  way, per the two-attempts rule. The mechanism this check would exercise — a stalled `send()`
  responding to the stop flag, and `Listener::run()`/the pool's destructor returning/joining
  promptly while a download is stalled — **is** covered by two targeted unit tests instead
  (`tests/test_fileserver.cpp`: "serveFile's stop flag aborts a stalled send instead of hanging";
  `tests/test_listener.cpp`: "stop() still returns run() promptly while a large download is
  stalled"), both green. Linux/macOS builds: still unverified (same as step 1 left them).
- **Decisions taken:** manifest schema matches the engine's existing format exactly (see above,
  not the assignment's invented field names) — avoids a step-3 translation layer. No JSON
  library added; a ~150-line hand-rolled parser scoped to this flat shape keeps the "no
  third-party code besides doctest" decision from step 1. `Content-Length` with a normal
  streamed body, not literal `Transfer-Encoding: chunked` — the file size is known upfront from
  one `file_size()` call, so chunked framing buys nothing; the Done-when criteria explicitly
  accept either. The worker pool is a private nested class (`Listener::Pool`) defined only in
  `Listener.cpp`, not a new public header, since nothing else needs it. `Http.h/.cpp` is a new
  shared pair (not in the assignment's file list) because `Listener.cpp` and `FileServer.cpp`
  both need the same response-building/send-loop code — duplicating it would have broken the
  Hive's own "no duplicates" rule. Pushed to a branch, not `main` (see above).
- **Decisions suggested, not taken:** step 3 (wiring the engine's `HE_ContentSync` to this
  server) should add an HTTP client call where `SftpClient`/`EngineContentSync` currently talk
  SFTP, keep the SFTP path as the documented fallback, and reuse the manifest parsing this step
  already matches field-for-field — no new parser needed on the engine side. Step 4/5 should add
  a CI workflow to the server repo (still missing, carried over from step 1) and verify
  Linux/macOS.
- **Pending:** no CI workflow yet for the server repo (Linux/macOS builds still unverified,
  carried over from step 1). The branch above is not merged — needs the queen's review/merge
  before step 3 can build on it.
- **Traps:** `Listener() = default;` written inline in the header does not compile once a member
  is a `unique_ptr` to a forward-declared nested type (`Listener::Pool`) — MSVC needs `Pool`
  complete to reason about the implicit constructor's exception safety, even though nothing in
  it can actually throw. Fix: declare (don't define) both the default constructor and the
  destructor in the header, define both out-of-line in the `.cpp` after `Pool`'s full definition
  — same pattern the scaffold already used for `~Listener()` alone. Also: `std::ifstream::read()`
  sets failbit on a short final read, so looping on `while (in.read(...))` silently drops bytes
  that were actually read if the file shrinks mid-request — check `gcount()` directly instead of
  the stream's bool conversion.
- **Next steps:** 1. Queen reviews/merges
  `claude/content-server-step-2-file-serving` on the server repo. 2. Step 3: engine-side
  `HE_ContentSync` client talks to this server (manifest fetch + `GET /content/...`), SFTP stays
  as fallback, reuse the matched manifest schema. 3. Carry forward: CI workflow for the server
  repo, Linux/macOS verification.
