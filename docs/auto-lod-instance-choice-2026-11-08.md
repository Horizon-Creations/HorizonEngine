# Auto-LOD step 5: level choice per instance (design only, NOT implemented)

Status: the bee was stopped by the queen before any code was written. This file is the survey and the
plan, so the next worker does not repeat the reading. No source file changed.

## What the code does today (verified by reading)

* `src/HE_Rendering/src/FoliageExtract.cpp`: `Levels::plain(d)` / `choose(d, prev, held)` / `whole(nearSq, farSq)`
  (lines ~241-279) take ONE number, the ground-plane (XZ) distance of the plant's origin to the camera, and compare it
  with `lv[k].from` (= `FoliageLod::distance`). The scale of the plant is not read anywhere.
* Callers: `extractPerInstance` (~329), `extractOrdered` (~367), `runStraddler` (~547), `planBuckets` (~476, buckets via `whole`).
* `from` is authored (`AutoLodApply.h` `fillFoliageLayer`) from `worldError * instanceScale` through `lodDistanceForError`
  (1080 px reference screen, 60 degrees, constant by design, see `LodDistance.h`). `instanceScale` is
  `0.5 * (minScale + maxScale)` of the layer (`AutoLodDialog.cpp:316`). The impostor start is
  `impostorStartDistance(plantHeight)` = distance where the plant is `kImpostorPixels` (128) tall, also at the mean scale.
* Plant scale is in the stored matrix: `FoliageSystem.cpp:255` draws a uniform scale in `[minScale, maxScale]` per plant.
* `FoliageLodMemory` keeps one level byte per bucket / plant; it is independent of how the level is chosen.

## Formula (decided in this survey)

Pixel height of a plant at ground distance d: `h * s * focal / d`. The stored thresholds are the distances at which the
MEAN-scale plant (`ref = 0.5 * (minScale + maxScale)`) reaches the level's pixel size. A plant of scale `s` reaches the same
pixel size at `d * ref / s`. So the whole per-instance test is: **`d_eff = d * ref / s`, then the existing `plain` / `choose`
on `d_eff`**. No format change (the thresholds stay metres at the mean scale), the card switch is the same test
(its `from` is the 128 px distance at mean scale, now applied per plant).

* `s` = length of the stored matrix's Y column (`glm::length(glm::vec3(m[1]))`), times `length(parent[1])` for
  `InstanceBlock::Parent::General`. Read from `layer.local[i]` next to `position(i)` (same cache line), clamp `>= 0.01`.
* Hysteresis stays in distance of `d_eff` (at mean scale). In pixel terms that is a constant fraction of the plant's pixel
  size (`h / from`), the usual ratio form.
* Buckets (`Levels::whole`): the plants of a bucket have `d_eff` in `[near * fLo, far * fHi]` with
  `fLo = ref / max(|minScale|,|maxScale|)`, `fHi = ref / max(0.05, min(|minScale|,|maxScale|))`. Widen the two inputs of `whole` by
  these factors (more straddlers near a boundary: ring of about 0.8..1.2 x `from` for scale 0.8..1.2; same as before when
  `minScale == maxScale == ref`).
* Replay key (`addToKey`, ~686): add `bitsOf(ref)`, `bitsOf(fLo)`, `bitsOf(fHi)`.
* `extractPerInstance` and `extractOrdered` ("distance alone" paths) get the same `d_eff`.
* Live camera fov / window size stay out, on purpose: `LodDistance.h` says the reference screen is a constant so a project's
  distances do not change with the editor's docking size. Using the live fov would be a separate decision.
* Dither / alpha blend between level and card: NOT done (needs a per-instance field in the instance layout on all backends).
* GPU culling / GPU level choice: not done. Plan doc section 2.3 (no indirect draw, no compute on macOS GL 4.1): the CPU extract
  stays the single path for all five backends.

## Test to write (tests/test_foliage_lod.cpp)

Two plants at the same ground distance, scale 0.8 and 1.2 in a layer with `minScale 0.8, maxScale 1.2` and one level at 40 m:
at 38 m the big one (`d_eff = 38 * 1.0 / 1.2 = 31.7`) stays level 0, the small one (`38 / 0.8 = 47.5`) takes level 1. Check in
all three paths (PerInstance, Ordered, Clusters straddler) and a hysteresis case (plant at the boundary holds its level over
several camera steps, `FoliageLodMemory` supplied).

## Handoff

* **Done:** survey and design above. No code, no build, no test run.
* **Not verified:** everything about the implementation.
* **Next steps:** 1. add `refScale / fLo / fHi` to `Levels` (filled in `levelsOf`), 2. `d_eff` in the four call sites, `whole` widened,
  key words, 3. new test cases, run `*test_foliage_lod*`, `*test_auto_lod*`, `*test_impostor*`, `*test_lod*`, `*test_foliage_cluster*`,
  `*test_culling*`, 4. update the comments in `AutoLodApply.h` (thresholds are mean-scale distances, the extractor scales per plant),
  5. dump witness (`HE_DUMP_FOLIAGETEST`, Metal and GL, compare the plant mask).
