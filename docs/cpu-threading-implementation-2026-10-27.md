# CPU threading: implementing the prioritized optimizations (Thema 238, Schritt 6)

27.10.2026, NN-WS03 (Windows, AMD Ryzen 9 9950X, 16C/32T), `C:/hb/cta5` (Release,
Ninja/MSVC, reused from Schritt 3/5). Builds on `cpu-threading-single-step-optimization-2026-10-27.md`
(Schritt 5, the three candidates below) and `cpu-threading-dependencies-2026-10-24.md` (Schritt 4).

## 1. What was implemented

### #1 — `levelOf` scratch reuse (`FoliageExtract.cpp`)

`runStraddler` allocated a fresh `std::vector<uint8_t> levelOf(b.count, kUnknown)` per
straddler-bucket test. `StraddlerResult` (already the per-task result buffer the pool
jobs write into, kept across frames via `StraddlerBuffers`) now carries a `levelOf`
member; `runStraddler` resizes it up (never down) and re-fills it with `kUnknown` up
to `b.count` instead of allocating. Same reuse pattern `r.mats`/`r.clusters` already use.

### #10 — Terrain LOD height/normal reuse (`TerrainMeshGenerator.cpp`/`.h`, `TerrainSystem.cpp`)

`TerrainSystem::buildChunk` built each of a chunk's `g.numLODs` levels (65, 33, 17, 9
verts/side) by independently bilinear-resampling the **same** height field and
finite-differencing a normal at every vertex. LOD(k+1)'s grid is an exact stride-2^k
subset of LOD0's, so a new `TerrainChunkLODCache` (one per chunk, local to the
`buildChunk` call) remembers the finest level's heights/normals (filled as a
byproduct of that level's own grid-fill loop, no extra pass), and a new
`generateTerrainChunkMeshLOD()` downsamples it for every coarser level instead of
resampling the field — falling back to the old resampling path (and refreshing the
cache) whenever the requested level's `vertsPerSide` is not an exact power-of-two
stride of what is cached.

**Why this is bit-identical, not just "close enough" — and why it needs the
power-of-two guard, not just "a stride":** `u` evaluates as `u0 + ((u1-u0) * i) /
(N-1)`. The multiply `(u1-u0) * i` rounds once to the nearest float; multiplying that
*already-rounded* value by a power of two is exact (a pure exponent shift, no further
rounding), so `round((u1-u0)*i) * 2^k == round((u1-u0)*i*2^k)` — which is exactly the
cached base grid's numerator at index `i*stride`. An arbitrary stride has no such
guarantee: `round((u1-u0)*i) * 3` is not in general equal to `round((u1-u0)*i*3)`, two
independently-rounded products of different real values. That is why `strideOf`
(TerrainMeshGenerator.cpp) rejects anything that is not an exact power-of-two stride
rather than any exact integer stride — the guard is load-bearing, not a conservative
extra one could safely relax. `sampleField`/`sampleFieldNormal` are pure functions of
`(heights, srcRes, u, v)`, so matching `u`/`v` bit for bit is the whole proof; the
surface triangles and skirt (`finishChunkMesh`, factored out of `buildChunkMesh`
unchanged) run on whichever grid was filled and do not care how it got there.

New tests in `tests/test_terrain.cpp`: `generateTerrainChunkMeshLOD`'s cached
downsample `memcmp`-matches `generateTerrainChunkMesh`'s direct resample across all
four LOD levels, three chunks (one at the `u1=v1=1` clamp edge, one with a
non-power-of-two-fraction span `u0=1/3,u1=2/3` so `(u1-u0)*i` does not land on a round
number for almost any stride), a non-flat/non-linear field — and `cache.n` is
asserted to stay at the finest level's size after every call, so the test actually
fails if the guard is relaxed and the downsample path is silently skipped. A second
test seeds the cache with an off-sequence 49-vertex grid (span 0.1–0.7, same reason)
and confirms a 17-vertex request (stride 3) falls back to resampling — and that the
fallback really overwrote the cache (`cache.n == 17`), not that it happened to
downsample a wrong-stride cache and get lucky.

### #3 — FoliageSystem TRS tightening: **not implemented**

Per Schritt 5's own correction (glm's `translate`/`rotate`/`scale` are already
specialized, ≈60 multiplies not ≈192 as the original draft estimated), the ceiling
here is "low single digits of percent" of `scatterLayer`'s cost — the lowest-priority
of the three, and the one most likely to trade measurable headroom for bit-identical
risk (a hand-rolled TRS would have to reproduce glm's exact term order to stay
bit-identical, not just equivalent). Left for a future step if ever worth it; **not**
a risk this step took and walked back — it was never started.

### Animation parallelization (added to this step's scope by the queen's ruling, see §4)

Schritt 5 measured `tickAnimation`'s crowd cost (3.82-4.00 µs/character, linear,
`AnimationEval.cpp`) and named the fix as "the real lever here is parallelization —
Schritt 2/6 scope, not Schritt 5's". Implemented in `AnimationSystem.cpp`: entities
`HE::poseMayBeKept` already classifies as having no `RootMotionComponent`, no
non-empty animation layer stack and no active IK run through `parallel_for` on the
pool. That is exactly the set `PoseFinalize::poseFinalize` takes through its
FK+IBM-only path (`composeModelMatrices`/`applyInverseBind` into `thread_local`
scratch and the entity's own `boneMatrices` — nothing shared, nothing belonging to
another entity). Root motion (writes the entity's `TransformComponent` and, with a
context, its physics body), a layer stack (per-component "already finalized this
frame" flags) and IK keep running on the calling thread, in view order, exactly as
before.

**Look-at ordering, checked rather than assumed:** a complex entity's IK look-at can
target another entity (`PoseFinalize.cpp:390-403`), which raised the question of
whether splitting "simple" entities out of view order changes what a look-at sees.
Read `applyIk`'s look-at path: it calls `HE::worldPositionOf(world, t)` on the
target — only `TransformComponent`, never `SkeletalMeshComponent::boneMatrices`.
Simple entities (by definition, no `RootMotionComponent`) never write
`TransformComponent` anywhere in `AnimationSystem::update`, so a look-at targeting a
simple entity sees exactly the position gameplay/physics already settled earlier in
the frame, regardless of whether that entity ran before or after the complex pass.
A look-at targeting *another* complex (root-motion) entity is unaffected too: complex
entities still run serially among themselves in the exact same view order as before
this change — only simple entities were pulled out, and they never produce a value a
look-at reads. (The pre-existing order-dependency between two complex entities — "do I
see this frame's root motion or last frame's" depending on view order — is unchanged,
not introduced by this step.)

Each entity writes its clip notifies into its own slot (`std::vector<HE::NotifyQueue>`,
one per entity in view order); the slots are concatenated back into the caller's
`notifies` queue in view order after both passes finish, so the result is the same
sequence the fully serial loop would have produced regardless of which entities ran
in which pass. `HE_LOG_THROTTLE` (now callable from worker threads for the first
time) is already race-free by construction — its per-call-site throttle state is a
`std::atomic<int64_t>` with `compare_exchange_strong`, and the log sink itself is
mutex-guarded (`Log.cpp`) — so no new logging race. `HE_ANIM_PARALLEL=0` /
`AnimationSystem::setParallelOverride(0)` forces the old serial path (same convention
as `HE_FOLIAGE_PARALLEL`).

New test in `tests/test_animation_bench.cpp`: a 400-character crowd, every third one
carrying a `RootMotionComponent` with `extractTranslationY` on (so the classification
actually splits the crowd, and — with a real `HE::RootMotionContext`, not `nullptr` —
root motion actually moves those entities instead of only extracting-and-locking a
delta nobody applies), ticked 5 frames twice — once with `setParallelOverride(0)`,
once with the pool — `boneMatrices` compared `memcmp`-equal, `TransformComponent`
compared equal (with an explicit check that at least one root-motion entity actually
moved, so the comparison cannot pass trivially at the default zero position), and the
merged notify queues compared entry-for-entry (entity, name, kind) in order.

## 2. Verification

**`he_tests` (`C:/hb/cta5`, Release, scratch `APPDATA`):**

| Suite | Result |
|---|---|
| `-tc="*foliage*" -tc="*Foliage*"` | 64/64 cases, 253201 assertions |
| `-tc="*terrain*" -tc="*Terrain*"` | 98/98 cases, 132157 assertions (incl. the 2 new LOD-cache tests) |
| `-tc="*animation*" -tc="*Animation*"` | 56/56 cases, 1969 assertions (incl. the new serial-vs-pool test) |
| Full suite, default (no `--no-skip`) | 5910/5910 cases, 3 of 4 repetitions; 1 repetition 5908/5910 |

**Trap, recorded as a lesson:** the first full-suite attempt used `--no-skip`, which
force-runs `test_auto_landscape_variants.cpp`'s `doctest::skip(getenv(...) ==
nullptr)`-marked "Dump the auto landscape shaders" case — a diagnostic dump helper
that `SIGSEGV`s without `HE_DUMP_AUTOLAND_SHADERS` set. doctest's crash handler
reports that one case and then **aborts the whole binary**, so the "377 cases, 1
failed, 5552 skipped" result from that run never actually executed the ~5550 cases
after it alphabetically — it was not a 376/377 pass rate, it was an early abort
reported as if it were a small, nearly-complete suite. The correct invocation for a
full run is **without** `--no-skip` (`he_tests.exe` alone), which lets doctest's own
skip markers take effect and actually exercises all 5910 non-skipped cases. Re-run
4 times that way: 3 were fully green (5910/5910); one showed 2 failures / 7 assertions
that did not reproduce on any of the 3 retries — consistent with this machine's known
pre-existing Windows flakes (clipboard/net/gamepad timing), not a regression from this
step's changes, but not pinned down to a specific case either (the failing run's
output was not captured to a file, only tailed).

DLL freshness checked by hash: `tests/HorizonScene.dll` beside `he_tests.exe` hashes
identically to the just-built `src/HE_Scene/HorizonScene.dll` (lesson 107's stale-DLL
trap did not bite here — both changed source files recompiled their owning target and
the post-build copy ran).

**Backends:** `HorizonEditor`, `HorizonGame` and `he_tests` all built from the same
CMake configure, which compiles D3D11/D3D12/Vulkan/OpenGL unconditionally on Windows
(no Metal on this machine — see the warning posted to the topic, and §4). CI run
[40530201379](https://github.com/Horizon-Creations/HorizonEngine/actions/runs/40530201379)
covers the Mac/Metal and Linux compile + test legs; check its result separately (it
was still running when this doc was written).

**Pixel A/B (foliage):** a real BEFORE deploy exists for this step (`out/deploy/Editor`
copied to `C:/hb/before/deploy/Editor` before rebuilding) — the first time this topic
has had one (Schritt 3 had to fall back to a same-binary A/B). First attempt used
`HE_DUMP_FOLIAGETEST` with no `HE_DUMP_FOLIAGELOD`/`HE_DUMP_FOLIAGEDIST` — checking
`EditorApplication.cpp:7619-7653,7569`, that witness's layer then has `fl.lods.empty()`
and an effectively unbounded draw distance, so every instance is a "whole" bucket
(`FoliageExtract.cpp:470-477`) and `runStraddler`'s `if (!P.leveled) {...; return;}`
early-out (`:501`) means `levelOf` is **never reached** — that first comparison was a
general extract regression check, not evidence for `#1` specifically. Re-ran with
`HE_DUMP_FOLIAGELOD=30:sphere,80:cube HE_DUMP_FOLIAGEDIST=150` (confirmed by the log:
"HE_DUMP_FOLIAGELOD 2 level(s)" and "drawDistance 150") and case C's close-in camera,
so instances near the draw distance or a LOD boundary land in straddler buckets and
`runStraddler`'s leveled branch — the one that owns `levelOf` — actually runs, on all
four Windows-available backends:

| Backend | Result |
|---|---|
| OpenGL | identical |
| D3D11 | identical |
| D3D12 | identical |
| Vulkan | identical |
| Metal | not available on this machine — see warning posted to the topic |

The authoritative bit-identical proof for `#1` is still
`tests/test_foliage_parallel.cpp`'s "straddlers on the pool give the single-threaded
result" pair (origin + rotated landscape, a camera path that "crosses level boundaries
and moves back over them" — exactly the leveled straddler path, compared pool vs.
single-thread exactly, not just visually) — already in the `*Foliage*` test run above.
The pixel A/B above adds real-GPU, real-editor-code-path confirmation on top of that,
once actually aimed at the leveled path.

Terrain's bit-identity is covered by the `memcmp` unit tests above, not a separate
pixel A/B (no terrain is in the `FOLIAGETEST` witness scene).

## 3. Measured improvement

Baselines (BEFORE) are a fresh same-machine capture against the pristine Schritt-5
binary (`C:/hb/before/`), not reused old numbers — this machine's Schritt-5 doc
numbers are close but were single samples; these are 2+ repetitions each.

**#1 `levelOf` reuse** — `he_tests` "Foliage extract bench" (524285 instances, moving
camera, median of 600, 3 repetitions each side):

| | one thread | pool |
|---|---|---|
| BEFORE | 0.479 / 0.487 / 0.488 ms | 0.241 / 0.248 / 0.249 ms |
| AFTER | 0.483 / 0.490 / 0.486 ms | 0.240 / 0.246 / 0.244 ms |

**No measurable change** — within the run-to-run noise on both sides, as Schritt 5
itself predicted ("small; allocator overhead only"). The straddler path this touches
is cheap enough (1.9% of per-frame WORK when memoized, Schritt 5 §2 item 1) that one
`vector` alloc/free per moving-camera bucket does not clear the noise floor at this
instance count. Safe, no regression, correctly scoped as a low-priority item.

**#10 terrain LOD reuse** — `HorizonGame`, `s512_x4` scene, `HE_PROFILE_WARMUP=1
HE_PROFILE_CAPTURE=5` (catches the one-time chunk build), `Terrain` scope, 2
repetitions each side:

| | BEFORE | AFTER |
|---|---|---|
| run 1 | 4.977 ms | 4.183 ms |
| run 2 | 4.973 ms | 4.173 ms |

**~0.80 ms faster (~16%)** for this scene's 16-chunk terrain build — real and
consistent across both repetitions, slightly under Schritt 5's "~20-30%" estimate
(the estimate was from a code read; the coarser levels' combined cost includes some
work — grid allocation, the skirt, triangle indices — that downsampling does not
touch). Streaming/load-time only, not per-frame.

**Animation parallelization** — `he_tests`, `HE_ANIM_BENCH=1`, same binary,
`HE_ANIM_PARALLEL=0` (serial) vs the pool (default), p50 of 240 ticks:

| characters | serial (BEFORE path) | pool (AFTER) | speedup |
|---|---|---|---|
| 1 | 0.004 ms | 0.004 ms | — |
| 50 | 0.197 ms | 0.195 ms | — (below `parallel_for`'s grain, stays on the calling thread) |
| 200 | 0.779 ms | 0.150 ms | **5.2×** |
| 500 | 1.932 ms | 0.194 ms | **10.0×** |
| 1000 | 3.860 ms | 0.331 ms | **11.7×** |

At 1000 characters (a crowd scene) this is 3.53 ms of main-thread time per frame
given back — far above either algorithmic item, and the one change in this step with
headroom to actually move a frame budget. Small crowds (50) do not regress: the
`parallel_for` call's `minGrain=32` keeps it on the calling thread until there is
enough work to be worth dispatching, matching the existing `HE_FOLIAGE_PARALLEL`
convention's own grain logic.

`s512_x4`/`ref10k` steady-state game captures show no animation signal (neither scene
has an animated crowd) and no regression on the denominator (`frame cpuMs − WaitFrame`
≈0.51 ms both before/after on `s512_x4`, ≈1.4 ms both on `ref10k`, within run-to-run
noise) — these two scenes exist as the regression guard the brief asked for, not as
evidence for the animation number (that is the crowd bench above, which is what
Schritt 5 was specifically asked to produce a number for).

## 4. Risks found during implementation

- **Animation was not in this step's original written assignment** (which named only
  the three algorithmic items and explicitly said "do not parallelize further"). Asked
  the queen (hive_request #151) before starting it; the ruling was that the
  Hive-written "do not parallelize further" referred only to Schritt 4's *rejected*
  fork-extract/sky-bake candidates, not to the animation parallelization Schritt 5
  itself had named as this step's job. Implemented on that ruling.
- **Cross-entity hazards ruled out the naive "just wrap the loop in `parallel_for`"
  approach**: root motion writes the entity's own transform/physics (safe, disjoint
  per entity) but a layer stack or active IK can read *another* entity's transform
  (look-at) and carries a per-component "already finalized this frame" flag — neither
  is safe under concurrent, unordered execution. Scoped the parallel path to exactly
  the entities `HE::poseMayBeKept` already proves touch nothing but their own
  components and `thread_local` scratch, and kept everything else serial.
- **Metal not available on this machine** (see §2, and the warning posted to the
  topic) — rely on CI's macOS leg for that backend's compile/test coverage; no pixel
  A/B exists for Metal from this step.
- **A separate, concurrent Hive topic (321/322, "Optimize src/HE_Scene/...") landed
  PRs into `release/0.7.0` touching `src/HE_Scene/src` and `src/HE_Scene/include`
  while this step was running** (seen via `hive_inbox`, not investigated further —
  out of this step's scope). The queen merging this topic should check for conflicts
  against whatever those PRs changed in `TerrainMeshGenerator.cpp`, `AnimationSystem.cpp`
  or `FoliageSystem.cpp`.

## 5. Next steps

1. Watch CI run [40530201379](https://github.com/Horizon-Creations/HorizonEngine/actions/runs/40530201379)
   for the Mac/Metal and Linux legs this step could not run locally.
2. If #3 (FoliageSystem TRS tightening) is ever taken up: it would need to reproduce
   glm's exact per-term arithmetic (not just an equivalent formula) to stay
   bit-identical, and the ceiling is low single digits of percent — probably not
   worth a step of its own unless bundled with other `scatterLayer` work.
3. Schritt 5's still-open items (a `CellStreaming` number via `make_cell_game_dir.py`,
   `make_land_game_dir.py`'s Windows `cp -cR` fix) remain open; this step did not
   touch them.
4. The queen should check topic 238 against topics 321/322's concurrent HE_Scene
   changes before merging into `release/0.7.0` (see §4).

## Handoff

- **Topic / step:** Thema 238, Schritt 6 (global step id 1203).
- **Branch / commit:** `claude/cpu-threading-audit-jobsystem-auslastung-daten-vorbereitung-`
  (pushed; this doc's revision is the latest commit at write time).
- **Done:** `#1` (`levelOf` reuse, `FoliageExtract.cpp`), `#10` (terrain LOD
  downsample, `TerrainMeshGenerator.cpp`/`.h` + `TerrainSystem.cpp`) and animation
  parallelization (`AnimationSystem.cpp`/`.h`) implemented, tested and measured; `#3`
  (TRS tightening) deliberately not implemented (see §1).
- **Files touched:** `src/HE_Rendering/src/FoliageExtract.cpp` (`StraddlerResult::levelOf`,
  `runStraddler`); `src/HE_Scene/include/HorizonScene/TerrainMeshGenerator.h` +
  `src/HE_Scene/src/TerrainMeshGenerator.cpp` (`TerrainChunkLODCache`,
  `generateTerrainChunkMeshLOD`, `strideOf`, `buildChunkMeshFromGrid`,
  `finishChunkMesh` extracted from `buildChunkMesh`); `src/HE_Scene/src/TerrainSystem.cpp`
  (`buildChunk`'s LOD loop); `src/HE_Scene/include/HorizonScene/AnimationSystem.h` +
  `src/HE_Scene/src/AnimationSystem.cpp` (`setParallelOverride`, the simple/complex
  split); `tests/test_terrain.cpp` (2 new cases); `tests/test_animation_bench.cpp`
  (1 new case, `populateMixed`, a notify on the bench clip).
- **Verified:** see §2 in full. Summary: `he_tests` foliage/terrain/animation suites
  green; full suite green on 3 of 4 default (non-`--no-skip`) runs, one unreproduced
  flake; pixel A/B bit-identical on OpenGL/D3D11/D3D12/Vulkan against a real BEFORE
  deploy, on a witness configured to actually exercise the leveled straddler path;
  terrain LOD bit-identity proven both mathematically (§1) and by `memcmp` test.
- **Not verified:** Metal (no Mac on this machine) — relies on CI's macOS leg, compile
  + `he_tests` only, no pixel A/B. The one full-suite flake was not pinned to a
  specific case (output not captured that run).
- **Decisions taken:** implemented animation parallelization despite it not being in
  the Hive's written assignment, on the queen's explicit ruling (hive_request #151) —
  Schritt 5 had named it as this step's job, the written "do not parallelize further"
  referred only to Schritt 4's already-rejected candidates. Did not implement `#3`
  (demoted priority, highest bit-identical risk for the implementation effort left).
  Scoped the animation parallel path to exactly `HE::poseMayBeKept`'s "simple" set
  rather than attempting the full crowd, after reading `RootMotionApply.cpp` and
  `PoseFinalize.cpp`'s IK/layer code and finding real cross-entity and physics-write
  hazards in the complex set.
- **Decisions suggested, not taken:** `#3` TRS tightening, if ever wanted, needs to
  reproduce glm's exact term order to stay bit-identical (not just equivalent) — flag
  this to whoever picks it up, do not let them re-derive the ≈192-vs-≈60-multiply
  estimate from scratch (Schritt 5 already did that correction).
- **Pending:** Schritt 5's still-open items (`CellStreaming` number, `make_land_game_dir.py`
  Windows fix) — out of this step's scope, not touched. CI run
  [40530201379](https://github.com/Horizon-Creations/HorizonEngine/actions/runs/40530201379)
  was still running at handoff time; its result was not yet known.
- **Traps:**
  - `he_tests --no-skip` is **not** "run everything" — it force-runs
    `doctest::skip(...)`-marked diagnostic cases too, including
    `test_auto_landscape_variants.cpp`'s `HE_DUMP_AUTOLAND_SHADERS` dump helper, which
    `SIGSEGV`s without that env var and takes doctest's whole process down with it,
    silently truncating the run to whatever ran before it alphabetically. For a real
    full-suite check, run `he_tests.exe` with **no** `--no-skip`.
  - `scripts/perf/foliage_pixel_ab.py` checks for a file literally named
    `HorizonEditor` (no extension) in each deploy dir — a Mac convention that does not
    exist on Windows (`HorizonEditor.exe`), so the script always reports "no
    HorizonEditor" there. Not fixed here (shared script, out of this step's scope); a
    local one-off script (pattern in `/c/hb/foliage_ab_before_after2.py`, not
    committed) worked around it for this step.
  - An `HE_DUMP_FOLIAGETEST` pixel witness with no `HE_DUMP_FOLIAGELOD`/`FOLIAGEDIST`
    never reaches `runStraddler`'s leveled branch at all (`fl.lods` empty →
    `P.leveled=false` → the early `if (!P.leveled) return;`) — set both env vars if a
    future pixel A/B needs to touch that code path specifically.
  - `C:/hb/cta5` is a shared build tree other bees reuse between steps; back up
    `tests/`, the target's own build-dir DLLs and `out/deploy/*` **before** rebuilding
    if a true BEFORE/AFTER comparison (not a same-binary one) will be needed.
- **Next steps:** see the numbered list immediately above this section.
