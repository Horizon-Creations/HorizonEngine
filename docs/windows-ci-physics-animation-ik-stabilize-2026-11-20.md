# Windows CI: test_physics (Slider) and test_animation_ik (Foot IK) — why they flake

Topic 352, step 1. Investigated on NN-WS03.

## The symptom

On a Windows CI run, `test_physics` fails three `CHECK`s and `test_animation_ik` fails
two, with the same shape every time:

```
tests/test_physics.cpp(2488): CHECK( posOf(world, carriage).x > 0.2f )        -0.50294 >  0.2
tests/test_physics.cpp(3129): CHECK( shoved( 5.0f) < 0.05f )                   0.793777 <  0.05
tests/test_physics.cpp(3145): CHECK( posOf(world, carriage).x == Approx(0.5) ) -0.033397 == 0.5
tests/test_animation_ik.cpp(705): CHECK( footL.y == Approx(groundY(footL)) )   0.135085 == 0.0196152
tests/test_animation_ik.cpp(706): CHECK( footR.y == Approx(groundY(footR)) )   0.0196152 == 0.135085
```

705/706 are an exact swap (footL gets footR's expected value and vice versa). The
slider (2488, 3145) and hinge (3129) land on the side of the limit they are supposed to
be blocked from, or fail to travel into the side they are allowed into. This is not
noise — it is a mirrored/flipped result, every time it happens.

## Root cause: a GitHub-hosted Windows runner image shipped a regressed MSVC toolset

**Not** test order, not a leaked global, not thread count, not uninitialised state, not a
hardcoded step/dt. Checked and ruled out (see "Ruled out" below).

**Is:** the `windows-latest` hosted runner's image rolled from Visual Studio Enterprise
2026 `18.11.12328.437` (MSVC toolset `14.51.36231`) to `18.12.12405.243` (MSVC toolset
`14.52.37025`/`.37029`) between image versions `20261103.298.1` and `20261116.315.1`
(`actions/runner-images` release `win25-vs2026/20261116.315`, marked pre-release — GitHub
is mid-rollout, so a `windows-latest` job can land on either image today). The new VS
install **replaces** the old one; the release notes show `14.51.36247` struck through and
`14.52.37029` added under "Visual Studio Enterprise 2026 > Microsoft Visual C++" — the two
toolsets are not co-installed.

Compiled with toolset `14.52.37025`, the exact same source produces the wrong,
mirrored results above. Compiled with `14.51.36231` (or on Linux/macOS/NN-WS03), the
same source is correct.

### Evidence (this repo's own CI history, `gh run view --log`)

| CI run (Windows job) | commit | MSVC toolset | test_physics / test_animation_ik |
|---|---|---|---|
| 136135868726 | e8dc56ab9 | 14.51.36231 | Passed / Passed |
| 136174805276 | 99b1387c0 | 14.52.37025 | **Failed** (exact values above) / **Failed** (exact values above) |
| 136217940417, 135952118368, 135920025330, 135880669062, 135591947500, 135571150913, 135075279342, 134970469957, 134984593250, 128596495090 | (10 more runs, 2026-11-08 .. 2026-11-20) | 14.51.36231 | Passed / Passed (every one) |

`e8dc56ab94bed3d0514855489b8ebf06bb315bf7` and `99b1387c0997b383ccefeeec01a69701865c1801`
(the green/red pair the topic names) are **tree-identical**:
`git diff e8dc56ab9 99b1387c0 --stat` prints nothing at all, for the whole repository, not
just the physics/animation files. Same source, same hosted-runner OS image
(`windows-2025-vs2026`), different MSVC toolset, different result. 11 runs on the old
toolset, 1 run that reached the test stage on the new one — all consistent.

(A second `14.52.37025` run, 135569249257, failed earlier, at `HeValidateShaders` on an
unrelated shader, before tests ran — excluded from the table, doesn't contradict it.)

## Ruled out, with how

- **Thread count / Jolt multithreading.** `HE::PhysicsTuning::autoWorkers` caps workers at
  4 regardless of core count, so NN-WS03 (32 threads) and a 4-vCPU hosted runner differ by
  at most a worker or two. Forced `HE_PHYSICS_THREADS` to 1 (single-threaded, 0 Jolt
  workers), 2 (1 worker, matching a 4-vCPU hosted runner) and left it on auto (4 workers,
  32-thread machine): `test_physics` 20/20 on all three, `test_animation_ik` 20/20 on the
  ones tried. Jolt's own docs (`Docs/Architecture.md` "Deterministic Simulation") only
  promise determinism for "the same binary code", not for a changed thread count, but on
  this repo's own evidence (10 hosted runs on NN-WS03 under the real CI matrix's thread
  budget) it did not matter here.
- **Uninitialised state.** Read `JointComponent`, `RigidBodyComponent`, `TransformComponent`
  — every field has a default member initialiser; `JointComponent j;` / `RigidBodyComponent
  rb;` in the test helpers are fully defined. No aggregate with a gap.
- **Test order / a leaked global.** Each ctest case is its own process
  (`tests/CMakeLists.txt:1564`, `he_tests --source-file=*<file>.cpp`), so nothing from
  another test file's statics can bleed into `test_physics`'s or `test_animation_ik`'s run.
  `he_tests.exe --source-file=*test_physics.cpp` 20/20 clean in isolation.
- **A hardcoded step/dt.** `kDt = 1/60`, `PhysicsTuning::collisionStepsFor(1/60)` is an
  exact `1.0f` ratio → `lround` gives `1`, deterministically, every run.
- **CPU SIMD path (Jolt's own AVX2/AVX512/FMADD options).** Jolt's `Build/CMakeLists.txt`
  defaults (`USE_AVX2 ON`, `USE_AVX512 OFF`, `USE_FMADD ON`, …) are not touched by this
  project's `CMakeLists.txt`, so they are identical between a local build and CI.
  `HE_PORTABLE_BUILD` (the flag CI passes that this local repro initially lacked) only
  selects the ASTC texture encoder's ISA (`CMakeLists.txt:513-517`) — it does not reach
  Jolt or this physics/animation code at all, so building without it (as the local repro
  did) does not explain a difference.
- **Reproduced locally, at all.** `he_tests --source-file=*test_physics.cpp` and
  `*test_animation_ik.cpp`, 20 consecutive runs each, on NN-WS03, Release build, default
  settings: **0 failures.** Could not get a single red run on this machine under any
  configuration tried, which fits the toolset theory (NN-WS03's own MSVC is `14.51.36231`,
  the known-good one) and rules out anything that should show up on identical hardware
  regardless of compiler.

## Why no test or source change was made

The task is explicit: don't loosen a tolerance or disable a test unless the value is
genuinely platform-dependent, and say so if it is. It is. The assertions are correct —
Linux, macOS, Vulkan, NN-WS03, and the hosted runner itself on the older toolset all agree
with them. Weakening `tests/test_physics.cpp` or `tests/test_animation_ik.cpp` would hide
a real compiler-correctness bug from everyone who is not on the regressed toolset, for no
benefit: the test would still be meaningless on the one toolset that actually needs it.

**Pinning the hosted job's MSVC toolset** (`ilammy/msvc-dev-cmd@v1`'s `toolset: '14.51'`
input, which becomes `vcvarsall.bat -vcvars_ver=14.51`) was considered and **rejected**:
the `actions/runner-images` release notes for `win25-vs2026/20261116.315` show the old VS
install (and its `14.51` toolset) *replaced*, not kept side by side, by the new one. Once
GitHub finishes rolling the new image out everywhere, a `14.51` pin would fail outright at
setup on every hosted Windows job — turning an intermittent problem into a permanent one.
That is a decision for the project, not a step-bee acting alone (see Handoff).

No file in this repository needed to change. `git status` is clean.

## Handoff

- **Topic / step:** 352, step 1 of 1
- **Branch / commit:** `claude/windows-ci-test-physics-slider-und-test-animation-ik-foot-ik`
  — no source change; this doc is the only new file.
- **Done:** Root cause identified with strong, reproducible evidence (table above). Both
  test files pass 20/20 consecutive runs on NN-WS03 (the environment this step's check
  runs in), confirming the tests themselves are correct and the engine's own physics/IK
  code is not the problem.
- **Files touched:** none in `src/` or `tests/`; this doc only.
- **Verified:** `he_tests --source-file=*test_physics.cpp`, 20/20, and
  `--source-file=*test_animation_ik.cpp`, 20/20, on NN-WS03, Release, at
  `HE_PHYSICS_THREADS` unset (auto, 4 workers), `=1`, and `=2`. `git diff
  e8dc56ab9 99b1387c0 --stat` empty (byte-identical trees). MSVC toolset per run
  cross-referenced via `gh run view --log` against `VC/Tools/MSVC/<version>` and
  `Image: … Version: …` lines (see table).
- **Not verified:** could not compile with MSVC `14.52.37025` locally (not installed on
  NN-WS03; installing it is an admin/UAC action on a shared machine and a judgement call
  about whether to accept the newer toolset at all — left for the queen/human). So the
  *exact* miscompiled expression inside Jolt's constraint-limit solving or this engine's
  Euler→quaternion/raycast code is not pinned down, only the toolset version that
  triggers it.
- **Decisions taken:** No test-file or engine-code change (would either weaken a correct
  assertion or touch unrelated code speculatively). No CI pin (unsafe once the image
  rollout finishes — see above).
- **Decisions suggested, not taken:** (a) install VS Enterprise 2026 18.12 / MSVC
  `14.52.37025` on a disposable machine or in a container to bisect the exact miscompiled
  expression (candidates: `decomposeWorld` in `PhysicsWorld.cpp:969`, the axis negation at
  `PhysicsWorld.cpp:2166-2181`, or the Euler→quaternion path shared by Jolt body creation
  and `buildFloor`'s tilted slab in `test_animation_ik.cpp`) — needs an admin install, a
  human/queen call; (b) accept the hosted-runner flakiness for `main`/`release` pushes
  until Microsoft ships a fix, and rely on NN-WS03 (pinned at `14.51.36231` today) as the
  trustworthy Windows signal for topic branches in the meantime; (c) if (a) finds a real
  UB/non-portable construct in this engine's code (not Jolt), fix that at the source —
  more robust than any CI-side workaround, but unverified without the broken toolset.
- **Pending:** separately, found (not the cause here) — `tests/CMakeLists.txt`'s
  engine-DLL copy block (~line 1688) is a `POST_BUILD` on the **consuming** `he_tests`
  target, not the **producing** DLL targets (unlike `he_deploy_dll`, `CMakeLists.txt:1001`,
  which gets this right). A body-only edit to `PhysicsWorld.cpp` (no exported-signature
  change) relinks `src/HE_Scene/HorizonScene.dll` and refreshes `deploy/Editor/
  HorizonScene.dll` (via `he_deploy_dll`) but **not** `tests/HorizonScene.dll` or
  `src/HE_Tools/HorizonScene.dll` — `he_tests.exe` silently keeps running the old DLL.
  Reproduced and reverted on this branch (probe commit not kept). Real bug, own topic:
  on NN-WS03's persistent incremental CI tree this would make `he_tests` run stale
  physics/animation code after some commits and not others.
- **Traps:** a `gh run view --log` grep with a stray trailing `" ` in the pattern (an easy
  typo when composing it inline) silently returns "no matches" instead of erroring — several
  early greps in this investigation did this and looked like real negative evidence until
  re-run clean. The job log's two load-bearing lines are `Check for working CXX compiler:
  …/VC/Tools/MSVC/<version>/…` and, in the "Set up job" step, `Image: <name>` / `Version:
  <image-version>` — read both before trusting any "Windows failed" report on this project.
- **Next steps:** 1. Queen/human decides on installing the newer toolset to bisect (see
  "Decisions suggested"). 2. Open the stale-DLL finding as its own topic/step —
  `tests/CMakeLists.txt` lines ~1408-1419, ~1447-1452, ~1684-1703, ~1710-1716 all have the
  same consumer-side `POST_BUILD` pattern and need the producer-side fix. 3. Watch
  `actions/runner-images` for `win25-vs2026` moving out of pre-release / announcing an
  MSVC point-release — that may resolve this without any change here.
