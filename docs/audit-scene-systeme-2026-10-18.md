# Audit-Optimierung: Scene-Systeme (Stufe 4)

Thema 218, from `docs/audit/AUDIT.md` Stufe 4 and `docs/audit/modul_scene.md`.

## Step 1: SCN-101, SCN-102, SCN-103 (animation drivers)

**SCN-103.** `findBracket` (`AnimationEval.cpp`) uses two methods that return the
same index. Below 64 keys it does the linear walk it always did. From 64 keys on it
uses `std::lower_bound` with the predicate `!(x >= t)`, which sends a NaN playhead
to the last key, the same as the walk does. The audit proposed `lower_bound` alone,
but measured that way it is a regression on hand-keyed clips (table below). The
binary search needs ascending times. glTF requires that, but nothing enforced it, so
the `.hasset` clip loader (`ContentManager.cpp`, `AssetType::AnimationClip`) now
logs one warning per clip whose times are out of order.

**SCN-101.** The clip, blend and state-machine drivers keep their poses in
`thread_local` scratch vectors. `assign` resets a vector and keeps its capacity. The
same applies to `PoseSource::evaluate` (blend-space poses, deltas, order),
`blendPosesN` (`tmp` is swapped, not moved) and the `model` matrices in
`poseFinalize`. The state machine keeps a pointer to the buffer that holds the
finished pose and does not move the buffer away. Scratch in `applyOneLayer`
(layer/ref/out) is unchanged.

**SCN-102.** This is the gate. `SkeletalMeshComponent::poseKey` is runtime only
and is not serialized: the serializer writes the component field by field. It holds a
hash (`HE::PoseKey`, `PoseFinalize.h`) of these inputs:

- the driver tag
- the clip and mesh UUIDs and pointers, plus `channels.data()` and `skeleton.data()`
  (a hot reload keeps the UUID but not the arrays)
- the joint count and the bits of the post-advance playhead
- for the blend, also `blendAlpha` and both durations

If the key is unchanged, sampling, FK and IBM are skipped. These still run as before:
playheads, notifies, sync graphs and transitions. The gated state-machine frame calls
`notifyCollectClip` itself, the same call `evaluate` makes for a clip.

The key's lifetime works like this:

- `poseFinalize` clears the key, and the driver stores the new key afterwards.
- So a second driver, or a Sequence (which poses through `poseFinalize`), invalidates
  it.
- `CinematicPreview.cpp` writes the bones directly, so it clears the key itself.

Some entities and states are never gated:

- entities with a `RootMotionComponent`, a non-empty layer stack or active IK
  (`HE::poseMayBeKept`)
- crossfades
- blend-space states, because their weights follow the parameters every frame

`smc.dirty` has no reader at the moment (checked with grep), so a gated frame that
does not set it loses nothing.

### Measurements (MacBook Air M5, Release, under load 7-10 from other bees)

`findBracket` microbenchmark (`/tmp/scn218_bench/bracket_bench.cpp`, not committed),
ns per call, uniform playheads, two runs:

| keys | linear | lower_bound | committed (walk < 64) |
|---:|---:|---:|---:|
| 2 | 1.7-1.9 | 4.1-4.9 | 1.4-2.2 |
| 8 | 6.6-7.3 | 14.0-14.3 | 8.2-8.6 |
| 32 | 21.1-21.5 | 22.4-27.2 | 18.7-21.7 |
| 64 | 37.8-39.3 | 25.5-36.1 | 38.6-40.2 |
| 300 | 186-192 | 34-50 | 53-55 |
| 1000 | 504-569 | 71.5 | 63-67 |

The in-engine benchmark was a temporary doctest case and is not committed:

- `AnimationSystem::update` with 200 animators, a 60-joint chain, and 3 channels × 300
  keys per joint, timed over 100 frames, 3 runs per side.
- The base side used this branch with the 7 animation `.cpp` files from `2a63303d`.

| | base (2a63303d) | this step |
|---|---:|---:|
| moving playhead | 15.1-16.3 ms/frame | 8.7-9.4 ms/frame |
| frozen playhead (dt 0) | 15.1-16.5 ms/frame | 0.03-0.07 ms/frame |

The moving case shows the effect of SCN-101 and SCN-103 together. 300 keys is the
favourable case for SCN-103, and a hand-keyed clip gains only the SCN-101 part. No
split between the two was measured.

## Step 2: SCN-301, SCN-401/501, SCN-402, SCN-601, SCN-604 (dirty and settled gates)

The rule for every gate in this step: skip only when the skipped work would write
exactly the values that are already there. Every change that the audit itself
flagged as a behaviour change was left out, or replaced by an exact form of it.

- **SCN-301** (`GameReplication.cpp` `advanceInterpolation`, `InterpState`): an entry
  settles when its path is a single point (`previous == current`) or the
  interpolation has arrived (`t == 1`). It is then skipped while the transform still
  holds the pose it wrote (`writtenPosition/Rotation`). The audit's `t >= 1` alone
  would never fire: the server sends every relevant entity on every tick (30 Hz),
  and the default delay of 0.1 s is longer than a tick. A new sample, a baseline, a
  re-bound net id (`unsettle`) and a new `structureEpoch` (a reparent changes
  `wireOrigin`) unsettle the entry. A script write is still overwritten.
  Deliberate difference: a resting entity now gets the sample itself, not
  `mix(x, x, t)`. That expression can land an ulp beside x, so the old code made a
  resting entity jitter by up to one ulp per frame. Dead entries are still not
  erased (a re-adopted id keeps its buffer, as before).
- **SCN-401/501** (`PropertyReplicator.cpp`): `declarationMatches` compares the
  declaration with the table's names in place, with the same order and duplicate
  rule as `collect`. Values are compared by index, straight out of the runtime or
  `ReplicatedVarsComponent`, with no `std::find`. A mismatch takes the old
  `broadcastTableFor` path. Left: `Runtime::replicatedVariablesOf` still returns a
  fresh vector per entity and frame (HE_Core). `joinedConnections()` and the table
  reap still run every frame (one vector and one hash lookup per table).
- **SCN-604** (`RopeTrailSystem.cpp`): no point copy without an attachment; the
  rebuilt MeshData is moved into the asset. The give-back sweep runs only when an
  owned entry was not matched by its rope this frame, using the sweep's own keep
  condition, so a swap of one rope for another still sweeps. The FNV hash is
  unchanged.
- **SCN-601** (`PhysicsWorld.cpp` step write-back): `writeBackMemo` holds, for a
  top-level body, the last Jolt pose and the transform it produced. When both still
  match, the body is skipped. This is exact even with a stale memo, because a
  top-level pose depends on the Jolt pose alone. Children keep the full path. The
  audit's active-body list was not used: it would let a script's write to a
  sleeping body stand. Statics are still visited every step for the reap. The memo
  is used only for bodies Jolt reports inactive (`!IsActive()`). Applied to every
  body, it cost 7% of a step with 3000 falling bodies (0.69 -> 0.74 ms); with the
  gate that case is within noise of no memo at all (0.70-0.73 vs 0.71-0.72).
- **SCN-402** (`LODSystem.cpp`): a parent's world matrix is composed once when a
  second sibling follows, and a top-level entity's single product is written out.
  These are the same products, in the same order, as `worldMatrixOf`. A new test
  pins bit identity on a level boundary. The audit's translation-only variant
  (`parentWorld * local[3]`) failed that test with 86 of 800 boundary decisions
  wrong. The world-level skip (still camera, nothing moved) was not done: before
  propagate there is no exact signal that a transform moved, because LOD runs
  before any propagate in the tick and many writers never set `dirty`.

Measurement: a temporary doctest file (not committed, a copy is in
`/tmp/scn218_ab/bench_scn218_tmp.cpp` on the Mac it ran on). Release, M5, load 2.5-3,
no `yes` hogs, three alternating runs per side, median ms per call. The base side is
this branch with the step's source files from `a6c52f6b`, and it was run with its own
`libHorizonScene.dylib` through `DYLD_LIBRARY_PATH`: `he_tests` loads the library
from the build tree, so a copied binary alone runs the new code.

| case | base | this step |
|---|---:|---:|
| SCN-301: client update + propagate, 2000 resting entities | 0.152 | 0.038 |
| SCN-401/501: `PropertyReplicator::update`, 500 idle entities, 4 vars | 0.33-0.35 | 0.13-0.15 |
| SCN-601: step + propagate, 3000 sleeping and 3000 static bodies | 0.32-0.33 | 0.15-0.16 |
| SCN-604: `RopeTrailSystem::update`, 500 unchanged ropes | 0.127-0.135 | 0.111-0.114 |
| SCN-402: 10000 chunks, runs of 250 per parent | 0.64-0.65 | 0.36-0.37 |
| SCN-402: 10000 top-level props | 0.44 | 0.36-0.37 |
| SCN-402: 10000 chunks, 40 parents interleaved one by one | 0.65 | 0.71 (+8%) |

propagate recomputed 2000 (SCN-301) and 3000 (SCN-601) matrices per frame on the
base side, and 0 on this step's. **The figures in the commit messages of 5680e2b4,
2bfdbee0, 8b62df9e and a1acf993 are too high**: they compare against a first baseline
taken at load 10 (for example "0.64 -> 0.11" for SCN-301). The table above is the
valid one. The interleaved LOD case is the worst case; TerrainSystem creates a
terrain's chunks in one loop (`TerrainSystem.cpp:577`), so they come in runs.

## Step 3: SCN-801, SCN-802 (terrain gates)

Same rule as step 2: skip only what would have written what is already there.

- **SCN-801** (`TerrainSystem.cpp` `updateTessellation`): the landscapes are
  classified first: Off (factor 1 or no distance), Busy (not built, `dirty`,
  `regionDirty`: `updateTerrains` owns the tick) and Refine. Only chunks of a
  Refine landscape get the `worldPositionOf` walk and a bucket entry. Chunks of an
  Off landscape give a refined level back inline (`emptyTessLevel` adds or removes no
  storage, so this is safe inside the view). Chunks of a Busy or gone landscape are
  left alone, as before. The buckets are `thread_local` and keep their capacity.
  The audit's "nothing active -> return" was **not** taken: the cleanup of a copied
  component (a chunk that carries another world's refined mesh, hooked into its
  `LODComponent`) runs for every chunk, and it can be needed while no landscape
  refines and this world owns no mesh. An early return would leave LODSystem drawing
  the other world's mesh. What is left with tessellation off is one pass over the
  chunk pool reading three fields.
  One deliberate difference: the build budget (2 per tick, shared) now goes to the
  landscapes in view order. Before, it went in `unordered_map` order, which was
  unspecified.
- **SCN-802** (`TerrainSystem.cpp` chunk material sync in `updateTerrains`): first
  the parents' default-material emplaces (the only emplace that can move the pool),
  then one pass over the chunks for all landscapes with a small (landscape, source
  pointer) lookup, and the parent's material is read in place instead of copied per
  landscape and tick. `same()` is unchanged. A chunk without a MaterialComponent
  gets a copy taken fresh per emplace, because each emplace can move the pool.
  The audit's part (3), a change gate on the source material, was **not** done:
  the Inspector, MCP and scripts write `materialAssetId` and the overrides straight
  into the component (the existing test in `test_terrain.cpp` does exactly that),
  so a revision would miss them. Left per tick: one `try_get` and compare per chunk.

Tests (all new cases are red under a mutation, each built and run):
- `test_terrain_tessellation.cpp`, three cases: a copied chunk component with
  tessellation off (unhooked, the other world's mesh not emptied), tessellation
  switched off without a rebuild, and four landscapes in the four states.
  - M1, the audit's early return: both of the first two are red.
  - M2, Busy treated as Refine: red at :544.
  - M3, a gone landscape treated as Off: red at :545.
- `test_terrain.cpp`, one case: two landscapes with different materials and
  overrides, plus a chunk without a MaterialComponent.
  - M4, the lookup pinned to the first landscape: red.

Measurement: the same recipe as step 2 (temporary doctest case, not committed; a copy
is in `/tmp/scn218_s3/bench_test_terrain_tessellation.cpp` on the Mac it ran on).
The base is `400370dd`, run with its own `libHorizonScene.dylib`. Release, M5, load
2.9-3.5, no `yes` hogs, low-power mode off, three alternating runs per side, median
ms per call over 300 calls. The landscapes are parented under a root entity.
Resolution 2049 still gives 16×16 chunks, so the large case is four landscapes.

| case | base | this step |
|---|---:|---:|
| SCN-801: `updateTessellation`, tessellation off, 1 landscape, 256 chunks | 0.042-0.045 | 0.0005 |
| SCN-801: same, 4 landscapes, 1024 chunks | 0.174-0.186 | 0.0031-0.0033 |
| SCN-802: idle `updateTerrains`, 1 landscape, 256 chunks | 0.0070-0.0072 | 0.0069-0.0072 |
| SCN-802: idle `updateTerrains`, 4 landscapes, 1024 chunks | 0.032-0.035 | 0.027-0.029 |

Idle `updateTerrains` includes the weightmap check and `syncSplines`. That is what
`SceneSystems.cpp:113` ticks. The material sync was already cheap, and with one
landscape there is no factor to remove. The refining case (tessellation on) was
not measured: it does the same work, with a lookup in place of the hash map.

## Handoff

### Step 3 (latest)

- **Topic / step:** 218, step 3 of 4 (Hive step 1075).
- **Commits:** 07a42442 (SCN-801/802), and this doc.
- **Files:** `src/HE_Scene/src/TerrainSystem.cpp` (`updateTessellation`, chunk
  material sync in `updateTerrains`). Tests: 3 new cases in
  `test_terrain_tessellation.cpp`, 1 in `test_terrain.cpp`.
- **Verified:** the four mutations above were each built and turned red. Terrain
  and tessellation suites are green. Full `ctest -j4` (Release, without the
  temporary benchmark): 302/303, 2 skipped. The one failure is `test_hpak` "Disk
  registry" (6 == 2), the same environment failure as in steps 1 and 2.
- **Not verified:** no editor or game run, and the `HorizonEditor` target was not
  built (only `he_tests`). Refining case not timed. Linux and Windows only through CI.
- **Decisions taken:** no early return in `updateTessellation` (copied components),
  and no change gate on the material sync (direct writers).
- **For the queen to decide:** a material change gate would need every writer of
  `MaterialComponent` (Inspector, MCP, HE::api, Lua/Python) to bump a revision.
- **Merge risk:** topics 180/182 touch `TerrainSystem.cpp` on their own branches.
- **Next steps:** step 4 (SCN-302 shared asset cache, SCN-701/702).

### Step 2

- **Topic / step:** 218, step 2 of 4 (Hive step 1092).
- **Commits:** 5680e2b4 (SCN-301), 2bfdbee0 (SCN-401/501), 8b62df9e (SCN-604),
  a1acf993 (SCN-601), 926e71cc (SCN-402), and this doc.
- **Files:** `GameReplication.h/.cpp`, `Net/PropertyReplicator.h/.cpp`,
  `RopeTrailSystem.cpp`, `PhysicsWorld.cpp` (Impl `writeBackMemo`, step write-back,
  `removeEntity`, `clear`), `LODSystem.cpp`. Tests: 2 new cases in
  `test_game_replication.cpp`, 1 in `test_net_property_replication.cpp`, 1 in
  `test_rope_trail.cpp`, 2 in `test_physics.cpp`, 1 in `test_lod.cpp`.
- **Verified:**
  - Every new test fails on base or under a mutation:
    - SCN-301's new test is red on the base binary.
    - PropertyReplicator with the duplicate rule removed: red (`tablesSent`).
    - Physics with every body treated as top-level: red (child test).
    - LOD with the translation-only path: red (86 wrong).
  - Touched suites green: replication, net_*, anticheat, cell_*, integrity, physics*,
    character, collider, raycast, rope_trail, lod, terrain_tessellation,
    transform_propagation.
  - Full `ctest -j4` (Release, without the temporary benchmark): 302/303, 2 skipped.
    The one failure is `test_hpak` "Disk registry" (6 == 2): 4 `.hasset` files in the
    user's EngineContentCache, and it passes with an empty `HOME`. This is the same
    environment failure as in step 1.
- **Not verified:** no editor or game run, and the `HorizonEditor` target was not
  built (only `he_tests`). Multiplayer was checked with the loopback rigs only, not
  over real UDP. Linux and Windows only through CI.
- **Decisions taken:** exact gates only. Not the audit's active-body list (SCN-601),
  not `t >= 1` alone (SCN-301), not the translation-only LOD position, and no
  world-level LOD skip.
- **For the queen to decide:**
  - SCN-402 world-level skip: it needs a transform write revision, and the writers
    that never set `dirty` make that a Transform-slice change (see `modul_scene.md`
    cross-module note on `worldMatrixOf`).
  - SCN-501 rest: `Runtime::replicatedVariablesOf` into a caller's buffer, or a
    revision counter (HE_Core).
- **Next steps:** step 3 (SCN-801/802 terrain gates). Then step 4.

### Step 1

- **Topic / step:** 218, step 1 of 4
- **Branch / commit:** `claude/audit-optimierung-scene-systeme-stufe-4` @ 4d85fc83
  (main change), the hybrid commit after it, and this doc.
- **Files touched:**
  - `AnimationEval.cpp`: `findBracket`
  - `AnimationSystem.cpp`, `AnimationBlendSystem.cpp`,
    `AnimationStateMachineSystem.cpp`: gate and scratch
  - `PoseFinalize.h`/`.cpp`: `PoseKey`, `poseMayBeKept`, key clear, `model` scratch
  - `PoseSource.cpp` `evaluate` and `AnimationPose.cpp` `blendPosesN`: scratch
  - `SkeletalMeshComponent.h`: `poseKey`
  - `CinematicPreview.cpp`: two key clears
  - `ContentManager.cpp`: warning for unsorted clip times
  - `tests/test_animationsystem.cpp`: 6 new cases at the end
- **Verified:**
  - `he_tests --source-file='*test_animationsystem.cpp'`: 46/46 cases.
  - Related suites: 307/307 cases (notify, root_motion, layers, ik, blend_space,
    sequence*, animator_host, state machine graph, scene_serializer, culling,
    mcp_tools_animator).
  - Mutation check: each of these turns exactly its own test red.
    - predicate `x < v`: the NaN probe
    - no notify walk on a gated state-machine frame: the lap test
    - `blendAlpha` dropped from the blend key: the blend test
  - Full `ctest -j4` (Release): 302/303, 2 skipped. The one failure is `test_hpak`
    "Disk registry" (6 == 2). It passes with `HOME=<empty temp dir>`. This is the
    environment failure already recorded in `docs/audit-core-fundamente-2026-10-17.md`,
    not this step.
- **Not verified:**
  - No editor run by hand: the Cinematic preview bracket, and a hot reload with a
    paused animator.
  - The `HorizonEditor` target was not built locally. Only `he_tests` was, and it
    compiles `CinematicPreview.cpp`. `SkeletalMeshComponent.h` reaches the editor and
    the renderer, and CI covers them.
  - Linux and Windows only through CI.
- **Decisions taken:**
  - Gate on exact inputs, not on `playbackSpeed == 0`.
  - Hash, not struct.
  - `thread_local`, because the systems are namespaces.
  - Hybrid search with a cutoff of 64, from the measurement.
- **Not done (from SCN-102):**
  - (1) Visibility/distance animation LOD. It changes visible behaviour (off-screen
    shadow casters) and needs the renderer's view data.
  - (3) A `playing` flag on the state machine. It would be a new serialized field
    with inspector and help entries.
  - Both are left for the queen to decide.
- **Known limitation:** an asset unloaded and reloaded at the same address with the
  same UUID would pass the key unchanged. The editor's in-place clip edits (notifies,
  root-motion flag) do not touch what a gated entity's bones depend on.
- **Next steps:**
  1. Step 2 (SCN-301/401/501/402/601 revision/settled flags).
  2. If the queen wants (1) above: a field such as `smc.animLodLevel` written by the
     LOD system. Root motion and notifies must keep running when posing is skipped.
