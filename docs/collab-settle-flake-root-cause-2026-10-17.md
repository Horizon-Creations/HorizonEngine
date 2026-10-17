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

## Handoff

- **Topic / step:** 208, step 1 of 2
- **Branch / commit:** claude/flaky-test-test-collab-controller-settle-auf-macos-ci, this doc only (pushed: yes)
- **Done:** Root cause of the Settle flake named and reproduced (lock-grant ordering race,
  publishTransform drops). It is already fixed by 5ee30ca0, which is on release/0.7.0 and this branch.
- **Files touched:** this doc. The test code is unchanged; the instrumentation was reverted.
- **Verified:** CI tallies above (gh api, "Run tests" step of the macOS job); local repro
  1-4 above; clean he_tests 90/90 plus ctest Passed.
- **Not verified:** no new CI run (nothing in the code changed); not run on Linux or Windows locally.
- **Decisions taken:** no fix in the test, because the cause is already closed and a
  change would be only cosmetic.
- **Decisions suggested, not taken (queen):** step 2 "Fix umsetzen" is moot for `:3871`.
  Either close the topic, or shrink step 2 to: (a) correct the comment at `:3871-3874` (cause
  = dropped sends, not a slow runner), (b) optionally make tickUntil's wall cap 30 s for the
  margin above. Watching 3 more macOS CI runs (the topic text asks for it) is already
  covered by the 31 green ones since 5ee30ca0.
- **Traps:** a shared TMPDIR across parallel copies (see above); `rm -f $VAR/*` is blocked
  by the harness check, so use a fresh directory per batch; `sleep_for(1ms)` ≈ 35 ms on this Mac.
- **Next steps:** 1. The queen decides between closing and the mini step 2.
  2. If step 2 goes ahead: comment at `tests/test_collab_controller.cpp:3871`, optionally
  `:3803` wall cap, then `ctest -R '^test_collab_controller$'` plus one CI run.
