# Audit fixes, Stufe 0 (Thema 212), 2026-10-17

Findings from `docs/audit/AUDIT.md` Stufe 0, fixed one by one with the existing tests as
the net. Step 1 (REN-804, EDT-401) is commit 27e7067f; its notes live in the Hive handoff
only. This file starts with step 2.

## Step 2 (release traps part 2)

- **Topic / step:** Thema 212, step 2 of 3 (release traps part 2)
- **Branch / commit:** claude/audit-fixes-korrektheit-und-release-fallen-stufe-0 @ 29d15980 (pushed: yes);
  commits 57caf258 (Core), 2100da9a (Net), a180e8db (Editor/D3D12/Audio), 29d15980 (ContentSync + AUDIT.md tick)
- **Done:** all eight findings fixed.
- **Files touched:**
  - `src/HE_Core/src/ContentManager/EngineDependencies.cpp` ~291 (CORE-203: fetch state in a `shared_ptr` the callbacks own)
  - `src/HE_Core/src/Types/TypeRegistry.cpp` ~330 (CORE-604: `structDefault(..., visiting)` in the array branch)
  - `src/HE_Core/src/Platform/Process.cpp` ~340-395 (CORE-501: one deadline for child + drain, job kill, `CancelSynchronousIo`)
  - `src/HE_Net/include/Net/BitStream.h` `BitReader::hasBytes`; `src/HE_Net/src/CollabSession.cpp` snapshot chunk, `readComponentBody`, `readTerrainBody`, `readStructuralBody`, asset chunk (NET-105/106)
  - `src/HE_Editor/BoneMaskPanel.cpp` `jointRow` (EDT-2007)
  - `src/HE_Rendering/src/Backends/D3D12/D3D12Renderer.cpp` `drawDebugLines` + members `debugVBuf/debugCBuf[k_frameCount]`, `kMaxDebugLines` (REN-204)
  - `src/HE_Scene/src/AudioEngine.cpp` `reapFinished`, called next to `AudioSystem::updateSpatial` in `GameApplication.cpp` and `EditorApplication.cpp` (play only) (SCN-112)
  - `src/HE_ContentSync/include/ContentSync/SftpCredentials.h` `SftpEndpoint::sessionTimeoutMs` (30 s), `SftpClient.cpp` `Session::open` (CSY-102)
- **Verified (Windows, Release tree C:/hw212):**
  - *Negative controls, fail without the fix:*
    - CORE-501: proc child `orphan-holder` waited the grandchild's 20 s and `timedOut` stayed false.
    - CORE-604: stack overflow.
    - CSY-102: still blocked at 15 s.
  - *Pass, but would also pass on the old code:*
    - CORE-203: completions fire after a cancel from another thread. No sanitizer, so the old UB may survive.
    - NET-106: the old code also rejects, through the truncated-read branch. "Before any allocation" is not observable in the test.
    - NET-105: negative control deliberately not run, since the old path allocates ~4 GiB.
    - Component/terrain/structural readers: covered by review only; the tests exercise only the snapshot path.
  - *Build and review only:*
    - EDT-2007: no BoneMask panel test exists.
    - REN-204: D3D12 headless dump, `HE_GPU_DEBUG=1`, 8 frames, `HE_DUMP_SELBOXTEST` + `HE_DUMP_SECONDARY`. Boxes and preview grid draw through the per-slot buffers, and the debug layer reported only the existing ClearRenderTargetView perf note. That proves the path works, not that the race is gone.
  - Full `he_tests` (one binary): 5421 cases, 5417 passed, 4 failed. The failures are the two known inspector-ui clipboard cases and two `EngineProfiler` cases. The profiler cases pass alone and with their file, so this is cross-file order dependence on the profiler singleton (candidate: the F9 start path in `Application.cpp`). Not checked on the base commit; CI's per-file ctest cannot see it.
  - `editor_help_audit.py --check`: 1205/1205. HorizonEditor and HorizonGame build.
- **Not verified:** Linux/macOS. The `orphan-holder` and SFTP tests run there for the first time in CI 39043434861 (still running).
- **Decisions taken:**
  - Reap only `at_end && !paused` voices, once per frame in play (game + editor). Edit mode is untouched, because the Audio Editor, SequenceSystem and WeatherAudio reap or stop on their own.
  - The SFTP timeout applies per blocking call (libssh2 semantics), so big downloads are not cut off. A login that times out now counts as transient.
  - One debug-line VB/CB pair per frame slot: one `DrawScene` per submission, and the preview flushes on both sides. More than 4096 lines are cut, not dropped.
- **Behaviour changes:**
  - A reaped audio handle reads as unknown: `getTime`/`getLength` 0, bus "".
  - `audio.resume(h)` on a finished one-shot no longer restarts it (miniaudio restarts at end).
  - On Windows, a child that leaves a pipe-holding daemon now returns `timedOut=true`, and the job kills the daemon. Nothing changes with `timeoutMs=0`.
- **Traps:**
  - Restoring a mutated file with `Copy-Item` keeps the old mtime, so ninja keeps the mutated `.obj` (Hive lesson 374).
  - `C:\` root is not writable for logs; write them into the build tree.
- **Next steps:**
  1. Read CI 39043434861. If red, check `test_process_runner` (orphan-holder) and `test_sftp_timeout` on Linux/macOS first.
  2. Step 3: EDT-604 (`##wx` in reference titles), EDT-1718 (double-encoded dash), EDT-307 (collab reload never effective).

## Handoff

- **Topic / step:** Thema 212, step 3 of 3 (visible defects EDT-604, EDT-1718, EDT-307)
- **Branch:** claude/audit-fixes-korrektheit-und-release-fallen-stufe-0 (commit and push: see the Hive result post)
- **Done:** all three are fixed, and AUDIT.md Stufe 0 is fully ticked.
- **Files touched:**
  - `src/HE_Editor/EditorReference.cpp:38` `labelOf` (EDT-604): the heading drops the `##` suffix, `sec.id` keeps it (F1 anchor), and the search text follows the heading.
  - `src/HE_Editor/CppClassEditorPanel.cpp:91` `stateFor` (EDT-307): path resolution and loading are now separate steps, `if (!loaded) loadBuf`, as in `ScriptEditorPanel`.
  - `:278` `reloadFromDisk`: exact key first, otherwise any class with that path as a half (compared with `fs::path ==`).
  - `:314` new `bufferText(path, header)`, the tests' read side.
  - `src/HE_Editor/TypeAssetPanel.cpp:543` (EDT-1718): `C3 A2 C2 80 C2 94` replaced by `\xe2\x80\x94`. A byte sweep over src/tests/scripts/EditorDeps found no other occurrence.
  - `tests/test_editor_help.cpp`: the reverse loop now checks that no heading contains `##`; a new case at :153 pins `Weather/Fog Density##wx` → "Fog Density".
  - `tests/test_cpp_class_editor.cpp` (new): reload of the header; a peer edit to the `.cpp` reaching a tab opened on the `.h`; an unrelated path returns false.
  - `tests/CMakeLists.txt`: CppClassEditorPanel.cpp, the two ImGuiColorTextEdit sources and their include dir, per-file `HE_IMGUI_ENABLED`.
- **Verified (Windows, Release tree C:/hw212):**
  - *Negative control (tests written first, built against the old code):* red as expected. 5 `##wx` headings plus the pinned case. Reload kept the old text, and `reloadFromDisk(Foo.cpp)` returned false.
  - *After the fix:* both files green. `editor_help_audit.py --check` 1205/1205. HorizonEditor builds.
  - *Full `he_tests` (one binary, scratch APPDATA):* 5424 cases, 4 failed, the same 4 as step 2 (2 inspector-ui clipboard, 2 EngineProfiler order-dependence).
- **Not verified:**
  - The dash in the running editor (no screenshot). The byte check of the line is the evidence.
  - Linux/macOS: CI (link in the Hive result).
  - "Unsaved edits are dropped on reload" is not tested: the test cannot type into the TextEditor.
- **Decisions taken:**
  - A reload drops unsaved local edits of the class (the audit's open question). This is safe because `EditorUI` draws a peer-locked tab inside `BeginDisabled` (`tabReadOnly`, EditorUI.cpp ~3976-4105), and it matches ScriptEditorPanel.
  - Local strip in `labelOf` rather than exporting `Help::visible()`. The latter would mean a change to `EditorHelp.h`, which most of the editor includes.
  - EDT-1718 only the dash. The comment and cleanup items (TutorialPanel.h:39-41, TerrainTools.cpp:2164, ThemeAssetPanel "Nine/ten roles", `styleValueRow` `key`/`const_cast`, TutorialSteps duplicate "7.") are deliberately left open.
- **Traps:**
  - A new include dir on `he_tests` recompiles all ~520 objects once.
  - The agents' Bash tool turns a double backslash into a single one, even inside a quoted heredoc. A Python edit meant to write the escape text `\xe2` therefore wrote the raw UTF-8 dash. Build the backslash with `bytes([0x5c])` or use the Edit tool, then check the bytes with `od -c`.
- **Next steps:**
  1. Read the CI run. If red, check `test_cpp_class_editor` first: TextEditor in the test target is new on Linux/macOS.
  2. The topic is complete; the queen merges into release/0.7.0.
  3. Optional: the remaining EDT-1718 items as their own small step.
