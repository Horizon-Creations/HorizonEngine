# Audit fixes, Stufe 0 (Thema 212), 2026-10-17

Findings from `docs/audit/AUDIT.md` Stufe 0, fixed one by one with the existing tests as
the net. Step 1 (REN-804, EDT-401) is commit 27e7067f; its notes live in the Hive handoff
only. This file starts with step 2.

## Handoff

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
