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
