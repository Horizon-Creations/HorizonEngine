# Tool Setup: Recheck keeps reporting the old "cmake not found" (Thema 231)

After Tool Setup installed cmake (or the compiler), the editor only saw it after a restart.

## Root cause

`resolveCmake()` (`src/HE_Scene/src/HcCodegen.cpp`, anonymous namespace before
`setBundledCmakeDir`) cached its answer in `g_resolvedCmake` / `g_cmakeResolved` for the whole
process, **an empty answer included**. The only thing that dropped it was `setBundledCmakeDir()`
with a *different* directory; the editor re-sets the *same* one on every Recheck
(`EditorApplication::startToolchainProbe`), which is a deliberate no-op. So the startup probe's
"cmake not found" survived the install, the re-probe after the install
(`ToolSetupDialog.cpp` `refresh()`, `recheckToolchain`) and the Recheck button.

The topic's PATH theory was only secondary on macOS: `ensureToolPathAugmented()` already adds the
Homebrew prefixes, and compiler detection is not cached at all (it is the throwaway configure in
`probeToolchain`), it just never ran because `probeToolchain` returned early on the stale empty cmake.

## Fix

- `resolveCmake(bool fresh = false)` returns the string by value; `probeToolchain()` calls it
  with `fresh = true`. Every probe (startup, Recheck, re-probe after an install) is a fresh look;
  `buildDylib` / `toolchainAvailable` still use the cache.
- The three cache globals and `setBundledCmakeDir` share `g_cmakeMutex` (export worker, probe
  thread and UI thread all reach them; before, a plain global with a returned reference).
- Windows: when neither the bundle nor `cmake` on PATH answers, `cmakeInKnownWindowsDir()` tries
  `%ProgramFiles%`, `%ProgramW6432%`, `%ProgramFiles(x86)%`, `%LOCALAPPDATA%\Programs` + `CMake\bin\cmake.exe`
  (winget Kitware.CMake / cmake.org MSI). A GUI process never sees the PATH change of an
  installer that ran after it started.
- No editor/dialog code changed; `HcCodegen.h` comment for `setBundledCmakeDir` updated.

## Handoff

- **Topic / step:** 231, step 1 of 1
- **Branch / commit:** `claude/bugfix-tool-setup-erkennt-neu-installierten-compiler-cmake-e` @ 30c94267 (+ this doc), pushed
- **Done:** a probe after an install now sees the new cmake (and so the compiler) without a restart.
- **Files touched:** `src/HE_Scene/src/HcCodegen.cpp` (resolveCmake, setBundledCmakeDir, probeToolchain),
  `src/HE_Scene/include/HorizonScene/HcCodegen.h` (comment), `tests/test_toolchain_recheck.cpp` (new), `tests/CMakeLists.txt`.
- **Verified (macOS, Release, `out/build/release`):** new test `test_toolchain_recheck` fails before the fix
  (`CHECK(after.cmakeFound)` false, version empty), passes after (also through `ctest -R test_toolchain_recheck`).
  `test_toolchain_quoting`, `test_toolchain_vswhere`, `test_hc_codegen_export_build`, `test_gamelogic_build` pass.
  Full `he_tests`: 5682/5685 cases pass; 3 failures in `test_hpak.cpp:1188` (`scanContentDirectory() == 2` got 6,
  deterministic, also via ctest) and `test_profiler.cpp:107,215` (recording state / frame count). Neither touches
  the toolchain code; **not checked against a baseline** (the main checkout's `he_tests` is stale and does not load).
- **Not verified:** the Windows known-dir fallback (compiled only by CI, install flow not exercisable here); the
  real Tool Setup click path in the GUI (no GUI run). CI run started: https://github.com/Horizon-Creations/HorizonEngine/actions/runs/39685427969
- **Decisions taken:** reset inside `probeToolchain` rather than a public `forgetResolvedCmake()` called from the
  editor (every caller gets it, no editor wiring to forget). Windows via known install dirs, not a registry PATH
  re-read (no `windows.h` in this file, much smaller).
- **Decisions suggested, not taken:** if a Windows machine has cmake in a non-standard dir installed mid-session,
  a registry PATH re-read (HKLM + HKCU `Environment`) would be the general fix; leave as its own step.
- **Pending:** Windows verification on NN-WS03 (install via Tool Setup, Recheck, expect green without restart) — keep that step open. Look at the `test_hpak` / `test_profiler` failures on the release branch.
- **Traps:** `resolveCmake` holds the mutex across `cmake --version` process spawns, so `setBundledCmakeDir` on the UI
  thread (Recheck click) can wait ~100 ms if an export is mid-resolve. The new test replaces process PATH and its
  guard re-probes on exit so later tests do not inherit the stand-in cmake; warm up (`ensureToolPathAugmented` runs once) before replacing PATH.
- **Next steps:** 1. Read the CI run for the Windows compile. 2. Run the Windows check above. 3. Triage test_hpak/test_profiler.

## Handoff — step 2, Windows verification (2026-10-26, NN-WS03)

- **Branch / commit:** same branch, on top of 1a0b54fa (docs-only); the Windows test case below is a new,
  not-yet-committed change to `tests/test_toolchain_recheck.cpp` at the time of writing.
- **CI run 39685427969 re-read:** the Windows job's overall "failure" is `runtime_size` (48.2 > 48.0 MB) and
  `test_python_scripting` (`HorizonPython.dll` load) — the Python-drift issue this topic's step 3 owns, confirmed
  via the job log (`gh run view 39685427969 --log-failed`). Everything toolchain-shaped compiled and passed:
  `test_toolchain_quoting`, `test_toolchain_recheck`, `test_toolchain_vswhere` all green. **But** at that commit
  `test_toolchain_recheck.cpp` had no `_WIN32` body at all (POSIX-only `#if`), so its CI "Passed" was a vacuous
  0-case pass — it never exercised `cmakeInKnownWindowsDir()`. That gap is what step 2 was for.
- **Added:** a `#if defined(_WIN32)` test case in `tests/test_toolchain_recheck.cpp` ("Toolchain Recheck (Windows):
  cmake installed into a known install dir is seen without a restart"). It locates this machine's real `cmake.exe`
  via `SearchPathA`, clears `PATH` and points `ProgramFiles` at a scratch dir (`ScopedEnvWin`, restores everything
  incl. re-probing on scope exit — same pattern as the POSIX `ScopedPath`), confirms `probeToolchain()` reports
  cmake missing, copies the real cmake.exe to `<ProgramFiles>\CMake\bin\cmake.exe` (what winget/the cmake.org MSI
  leave behind), re-probes the way Recheck does, and checks `cmakeFound`/`cmakeVersion` AND `toolchainAvailable()`
  (the cached call `buildDylib` reads) are now true/non-empty. Uses the real system cmake, not a stub, so the
  known-dir *and* the subsequent `cmake --version` spawn are both exercised for real.
- **Verified (Windows, Release, `C:/hw231`, DEPLOY_DIR-isolated worktree build):**
  - `ctest -R test_toolchain_recheck`: green, 6/6 assertions (verbose run via `he_tests.exe --test-case=...`).
  - **Negative control:** commented out the one line `s_cmake = cmakeInKnownWindowsDir();` in `resolveCmake()`,
    rebuilt `he_tests` (incremental), re-ran the same test case: 3 of 6 assertions now fail
    (`after.cmakeFound`, `after.cmakeVersion` non-empty, `toolchainAvailable()`), confirming the test actually
    exercises the fallback and not something else. Restored the line (`git diff` on `HcCodegen.cpp` clean again),
    rebuilt, green again.
  - **Full `he_tests` (scratch `APPDATA`, vcvars64 loaded, `ctest -C Release`, 333 cases, 442 s):** 332/333 pass.
    Only failure: `test_inspector_ui` (clipboard copy/paste assertions) — matches the pre-existing, documented
    clipboard-access flake on this shared desktop session (`OpenClipboard`/`GetClipboardText`), unrelated to the
    toolchain change. **`test_hpak.cpp:1188` and `test_profiler.cpp:107/215`, which failed on macOS in step 1, both
    PASS here** — platform-specific to the macOS run, not a cross-platform regression. A first pass of this run
    (no `vcvars64` loaded, `APPDATA` set via a `cmd /c "set APPDATA=... && ..."` one-liner) additionally showed
    `test_contentmanager` and `sdk_example_project` as failed — both were artifacts of that setup, not the code:
    `sdk_example_project` needs `rc.exe` (resource compiler), only on PATH after `vcvars64`; re-run with vcvars
    loaded and `$env:APPDATA` set directly in PowerShell (not via a chained `cmd /c set ... && ...` string) fixed
    both. If you see either of those two fail again, suspect the harness before the source.
  - `test_tool_setup_ui.cpp` (pre-existing, unchanged) already covers `ToolSetupDialog.cpp`'s wiring — the
    `recheckToolchain` callback firing once when `toolchainInstallDone` flips — against a mocked `AppContext`;
    it passed in the same full run, so the dialog-level wiring and the engine-level fix now both have real
    coverage, just not connected to each other in one test.
- **Not verified:** the live GUI Recheck click path (no `horizon-editor` MCP connection available this session —
  "Tool Status / toolchain probe is NOT exposed" over MCP per an earlier session's notes anyway) and a real
  `buildDylib`/GameLogic compile picking up the Recheck result end-to-end in the running editor. The new test
  covers the same code `buildDylib` reads (`toolchainAvailable()`) right after a fresh probe, which is the
  strongest verification reachable without driving the GUI.
- **Gap flagged, not fixed (out of this step's scope per its Limits):** `cmakeInKnownWindowsDir()`'s known dirs
  (`ProgramFiles`, `ProgramW6432`, `ProgramFiles(x86)`, `LOCALAPPDATA\Programs`, each `+ "\CMake\bin\cmake.exe"`)
  do **not** include Visual Studio's own bundled cmake
  (`<ProgramFiles>\Microsoft Visual Studio\18\<edition>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`,
  confirmed present in CI's own Windows runner `PATH` dump). A machine with only VS's bundled cmake (no winget/MSI
  cmake, no cmake in the editor's own PATH) would still probe "not found" after a Recheck. A registry PATH
  re-read (already on this doc's "suggested, not taken" list) or one more known-dir entry would close this.
- **Next steps:** 1. Commit the Windows test case, push, start CI (`gh workflow run CI`) to confirm the new
  `_WIN32` body compiles and passes on a clean Windows runner too (the run referenced above predates this test).
  2. If anyone wants the VS-bundled-cmake gap closed, that is a new small step, not a reopen of this one.
