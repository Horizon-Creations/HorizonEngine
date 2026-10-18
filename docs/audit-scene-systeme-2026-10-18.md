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

## Handoff

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
