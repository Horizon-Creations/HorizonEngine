# Collab "Settle" flake on macOS CI: root cause (topic 208, step 1)

Case: `tests/test_collab_controller.cpp:3783` "Settle: a client's save and a quiet scene
reach the host's keep step", red on macOS CI at `:3871`
(`REQUIRE(tickUntil(... anna.settlePending(kSceneKey) ..., 600000))`).

## Result

**Root cause: an ordering race in the test, not slowness. It was already fixed by
5ee30ca0 (15.10., 15:54 CEST), and the CI runs this topic was opened on predate that fix.**

`CollabController::publishTransform` (`src/HE_Editor/CollabController.cpp:2579`) returns
without sending unless the *client's own* lock table holds the grant. Before 5ee30ca0 the
test waited only for the *host's* view (`anna.lockFor(subject)->owner == bob`), which is
true one update before Bob has read the grant. The five transforms after that are sent back
to back, with only an `anna.update/bob.update` between them and no retry. If the grant
reached Bob after all five, every send was dropped. Anna then never applied a change, the
scene never went pending, and `tickUntil` ran into its 10 s wall cap. That matches CI's
`test_collab_controller ... Failed 19 s` (10 s wait plus the rest of the file).

acdd0101 had raised that wait's bound from 5000 to 600000 sim-ms. That could not help,
because the transforms it waited for were never sent. 5ee30ca0 added
`REQUIRE(tickUntil(bob.ownsLock(subject)))` (`:3858`) before publishing, and that closes the
race. The comment at `:3871-3874` still blames "a slow CI runner". The CI job ran the whole
file in 19 s, so the runner was not slow; the comment is misleading.

## Evidence

**CI (macOS job, step "Run tests"):**

| Window | Runs | Red in Settle `:3871` | Other red |
|---|---|---|---|
| acdd0101 .. 5ee30ca0 (15.10. 00:00-13:55 UTC) | 11 | 4 (38716567591, 38723420944, 38731406474, 38737026808) | 0 |
| before acdd0101 (14.10., df80c19e) | - | 1 (38635995907, same assertion, bound 5000) | - |
| after 5ee30ca0 (to 17.10. 20:00 UTC) | 32 | **0** | 1 (test_physics_budget, 38909214550) |

At the pre-fix rate (4/11), 32 clean runs would have about a 6e-7 chance.

**Local (Release he_tests, this Mac, other bees building and testing at the same time).** I
used temporary getenv-gated switches in the test, reverted before the commit:

1. *Mechanism, current race width:* without the `ownsLock` wait and with a trace of
   `bob.ownsLock` before each of the five sends, send 0 was dropped in 15 of 16 runs (16
   parallel copies plus 12 `yes` CPU hogs). Sends 1-4 always passed, and all runs were
   green. The grant reaches Bob exactly one update after Anna sees it. Before the fix the
   test lived on the four remaining iterations being long enough for loopback delivery.
   On the CI VM they were not.
2. *Exact CI signature, deterministic:* without the wait AND without the updates between
   the sends (the grant arrives after the loop), 4/4 runs failed with all five sends
   dropped. The failure is the same `REQUIRE(settlePending)` after 239 ticks = 10.0 s wall.
3. *Current code under load:* Settle case 32/32 green (2x16 copies, 12 hogs). Bob owns the
   lock on all five sends every time. The whole file, 8 copies with 8 hogs and 10 copies
   with 12 hogs, each copy with its own TMPDIR: 18/18 green (90/90 cases each).
4. Clean (instrumentation reverted): `he_tests --source-file='*test_collab_controller.cpp'`
   90/90, `ctest -R '^test_collab_controller$'` Passed (44 s).

## Remaining timing margin in the Settle case (not a bug, for the record)

On this Mac `std::this_thread::sleep_for(1ms)` takes 33-39 ms (timer coalescing, on AC,
under concurrent load). So one `tickUntil` tick costs about 40 ms wall, and the 200-tick
wait (`:3883`) uses up to 8.1 s of its 10 s cap. Truncating it is harmless: the
`CHECK_FALSE` only gets weaker. The 120-tick quiet window (`:3877`) always ran its full 120
ticks (at most 4.8 s). It would only break the following Idle `REQUIRE` (`:3881`, bound
1000 sim-ms) above about 125 ms per tick, about 3x today's worst case. The first subcase's
`tickUntil(settlePending, 5000)` (`:3839`) is a network wait bounded by sim ticks. It took
at most 1 tick here and was never red in CI after the fix.

## Trap found on the way

Running several copies of `he_tests --source-file='*test_collab_controller.cpp'` in
parallel with a shared TMPDIR makes 7 of 8 copies red, in cases other than Settle (`:1531`,
`:2337`, `:2431`, `:3648`, `:3705`, `:3777`...). The cause is the fixed file names in
`fs::temp_directory_path()` (e.g. `:900`, `:1554`). Each copy overwrites the others' files.
Even a 6x wall budget does not help. ctest gives each test its own TMPDIR
(`tests/CMakeLists.txt` ~1350), so CI is not affected. For load tests of this kind, set
`TMPDIR=<own dir>/` per copy.

## Step 2: margin fix and verification

Commit 5a8d5713 changes only the Settle case's `tickUntil` (`tests/test_collab_controller.cpp:3801-3815`) and two calls:

- The comment at `:3875-3879` gives the real cause (no transform sent, the grant race), not "slow runner".
- The wall cap goes from 10 s to 30 s for network waits (`:3807`).
- New third parameter `wallCapped = true`. The two `CHECK_FALSE` quiet windows (`:3884`, `:3890`)
  pass `false`: they run their full 120/200 sim ticks, because a truncated window would leave the
  clock short of `kIdleMs` and fail the Idle `REQUIRE` at `:3888`.

Local results, Release he_tests, every copy with its own TMPDIR, another bee active on the device:

| Run | Result |
|---|---|
| 8 copies + 6 `yes` hogs | 8/8 green (90/90) |
| 10 copies + 10 hogs (load 11) | 10/10 green |
| 12 copies + 6 hogs | 12/12 green |
| solo, sequential | 8/8 green |
| freshly copied binary, first start | 3/3 green |
| **first solo run right after linking** | **89/90, 1 case red, log not kept; case unknown** |
| `ctest -R '^test_collab_controller$'` | Passed, 44 s |
| full `ctest -j4` | 298/299 that ran (2 skipped); only test_hpak red (`:1186`, 6 == 2), Passed again with `HOME=<empty dir>` (known local issue) |

So 41 of 42 runs were green. The one red run was never reproduced in 41 attempts (not even on a
fresh binary's first start). The change only lengthens waits; it can make a `CHECK_FALSE` stricter,
but it cannot cut a wait short. So the red case was most likely one of the known load flakes in other
cases of this file (Hive lessons: the "Larger assets" re-dial `pumpUntil`, the `openSession` REQUIRE).
Next time, keep every log from the first run on.

## Handoff

- **Topic / step:** 208, step 2 of 2
- **Branch / commit:** claude/flaky-test-test-collab-controller-settle-auf-macos-ci, 5a8d5713 (test) + this doc (pushed: see the Hive post)
- **Done:** comment corrected; wall cap 30 s on network waits; quiet windows sim-bounded only. Root-cause fix stays 5ee30ca0 (step 1).
- **Files touched:** `tests/test_collab_controller.cpp` (Settle case only), this doc. Nothing in `src/`.
- **Verified:** table above (local, macOS, Release).
- **Not verified:** Linux/Windows locally; the one red solo run's case (log lost).
- **CI:** one `workflow_dispatch` run on this branch (link in the Hive post). `ci.yml` has
  `concurrency: cancel-in-progress` per branch, so three runs cannot be queued at once: each would
  cancel the one before it. Runs 2 and 3 have to be started one after another, after the previous
  run has finished. Besides that, step 1 counted 32 green macOS runs since 5ee30ca0.
- **Decisions taken:** fix only the test, no change in CollabController/CollabSettle (the cause is closed).
- **Traps:** `wait` without PIDs in a load script also waits for the `yes` hogs and hangs; wait on
  the test PIDs only. test_hpak `:1186` red locally unless `HOME=<empty dir>`.
- **Next steps:** 1. Queen: after the dispatched run goes green, start runs 2 and 3 one after
   another (or accept the 32 earlier ones). 2. Merge into release/0.7.0.

