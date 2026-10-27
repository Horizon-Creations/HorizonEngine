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

**Why this is bit-identical, not just "close enough":** a grid vertex `i` of a
`N`-vertex level and the cached base's vertex `i·stride` (where `base.n - 1 =
stride·(N-1)`) compute `u = u0 + (u1-u0)·float(i)/float(N-1)` from exact integer
ratios that reduce to the *same real number* either way; IEEE-754 division is
correctly rounded to the nearest representable value of that real number, so the two
expressions produce the identical `float`, bit for bit — and `sampleField`/
`sampleFieldNormal` are pure functions of `(heights, srcRes, u, v)`. The surface
triangles and skirt (`finishChunkMesh`, factored out of `buildChunkMesh` unchanged)
run on whichever grid was filled, so they do not care how it got there. The
power-of-two check is load-bearing, not conservative: a stride of 3 (49→17 verts, for
example) would **not** reduce to the same rounded value, so a non-power-of-two stride
correctly falls back to resampling (covered by a dedicated test).

New tests in `tests/test_terrain.cpp`: `generateTerrainChunkMeshLOD`'s cached
downsample `memcmp`-matches `generateTerrainChunkMesh`'s direct resample across all
four LOD levels, two chunks (one at the `u1=v1=1` clamp edge), a non-flat/non-linear
field; a second test seeds the cache with an off-sequence 49-vertex grid and confirms
a 17-vertex request (stride 3) falls back correctly instead of misreading the cache.

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
frame" flags) and IK (look-at can read *another* entity's transform) all keep
running on the calling thread, in view order, exactly as before. Each entity writes
its clip notifies into its own slot (`std::vector<HE::NotifyQueue>`, one per entity in
view order); the slots are concatenated back into the caller's `notifies` queue in
view order after both passes finish, so the result is the same sequence the fully
serial loop would have produced regardless of which entities ran in which pass.
`HE_ANIM_PARALLEL=0` / `AnimationSystem::setParallelOverride(0)` forces the old serial
path (same convention as `HE_FOLIAGE_PARALLEL`).

New test in `tests/test_animation_bench.cpp`: a 400-character crowd, every third one
carrying a `RootMotionComponent` (so the classification actually splits the crowd),
ticked 5 frames twice — once with `setParallelOverride(0)`, once with the pool —
`boneMatrices` compared `memcmp`-equal, `TransformComponent` compared equal, and the
merged notify queues compared entry-for-entry (entity, name, kind) in order.

## 2. Verification

**`he_tests` (`C:/hb/cta5`, Release, scratch `APPDATA`):**

| Suite | Result |
|---|---|
| `-tc="*foliage*" -tc="*Foliage*"` | 64/64 cases, 253201 assertions |
| `-tc="*terrain*" -tc="*Terrain*"` | 98/98 cases, 132128 assertions (incl. the 2 new LOD-cache tests) |
| `-tc="*animation*" -tc="*Animation*"` | 56/56 cases, 1968 assertions (incl. the new serial-vs-pool test) |
| Full suite, `--no-skip` | 376/377 cases, 86030/86030 assertions passed |

The one full-suite failure, `test_auto_landscape_variants.cpp` "Dump the auto
landscape shaders" (SIGSEGV), is **pre-existing**: it crashes identically on the
unmodified Schritt-5 binary preserved at `C:/hb/before/tests/he_tests.exe` (built
before any of this step's commits). Not a regression; not diagnosed further here
(out of scope, matches an existing lesson about this test's Windows-only diagnostic
dump helper).

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
has had one (Schritt 3 had to fall back to a same-binary A/B). Compared the
`HE_DUMP_FOLIAGETEST` witness (100k instances, straddling the draw distance — exactly
the code path `#1`'s `levelOf` reuse touches) between the BEFORE and AFTER deploys on
all four Windows-available backends:

| Backend | Result |
|---|---|
| OpenGL | identical |
| D3D11 | identical |
| D3D12 | identical |
| Vulkan | identical |
| Metal | not available on this machine — see warning posted to the topic |

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
