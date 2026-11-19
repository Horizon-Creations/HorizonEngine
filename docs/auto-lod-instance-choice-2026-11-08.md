# Auto-LOD step 5: level choice per instance (implemented 2026-11-08)

Status: implemented as designed below (survey first, then code). The Handoff at the end is the current one.

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

* **Topic / step:** 331, step 5 (level choice and culling per instance).
* **Done:** `FoliageExtract.cpp`: `Levels::seen(d, plantScale)` (= `d * refScale / plantScale`), `refScale/factorLo/factorHi` filled in
  `levelsOf`, `plantScaleOf(block, i)` (Y column, times the parent's for a General parent). Used in `runStraddler` (with
  `choose`/hysteresis), `extractPerInstance`, `extractOrdered`; `Levels::whole` widens near/far by the factors, so a bucket only
  stays whole when every plant of any scale in the layer's range is clear of the band; replay key has the three new words.
  Comments in `FoliageComponent.h` (`FoliageLod::distance` is for a mean-scale plant) and `AutoLodApply.h` (the 128 px distance
  is the mean plant's; the extractor scales it per plant). Card = last level, so it uses the same test.
* **Tests (Release, `out/build/release/tests/he_tests`):** `*test_foliage_lod*` 13/13 (3 new: same distance different heights
  pick different levels in Clusters/PerInstance/Ordered; one-scale layer keeps the plain rule and whole buckets; a 0.6 m wobble on
  a boundary flips 0 plants with memory and >0 without), `*test_auto_lod*` 9, `*test_impostor*` 6, `*test_lod*`, `*test_foliage_cluster*` 27,
  `*test_culling*` 78, `*test_graph_instancing*` 7, `*test_layer_mesh_instancing*` 7: green. Full `he_tests`: 5993/5994, the one red is
  `test_hpak` "Disk registry: scanContentDirectory ..." (10 != 2), known on this Mac in every worktree, unrelated.
  `test_foliage_lod.cpp`'s `expected()` and the hysteresis case now use the test's own `seenDistance`.
  `test_layer_mesh_instancing.cpp` "four times the plants draw in the same number of calls": the depth-pass equality (draws and mesh
  set equal between 1x and 4x) was relaxed to "mesh set may grow, draws within +3": a rock of scale 0.6 in a 0.6..2.4 layer is as small
  as a mean rock at 2.5x the distance and takes level 2 at 30 m, so that sparse level shows in the shadow pass of the 4x scene only
  (seen: mesh 0x116 extra). The main-pass equality is unchanged.
* **Not verified:** no dump witness (Metal/GL): `HE_DUMP_FOLIAGETEST` has a layer WITHOUT levels, which this change leaves
  bit-identical by design (`leveled == false` path untouched), so a plant-mask A/B there would show nothing; a levelled scene needs
  the Foliage-Testszene with baked chains (outside the repo). No perf measured, none claimed. Editor not run. D3D/Vulkan untouched.
* **Decisions taken:** formula and constants above; reference screen stays 1080 px / 60 degrees (no live fov); ground-plane (XZ)
  distance kept; hysteresis in metres of the mean-plant-equivalent distance (constant fraction of pixel size). Preset layers
  (`LayerMeshPresets.h`, hand-set distances for the mean plant) now switch small plants earlier and big ones later: intended.
  CPU is the single path on all five backends: no backend has indirect draw / compute culling (plan 2.3; macOS GL 4.1 has no compute),
  so no GPU culling was added; the frustum / bucket culling stays as it was (step 6 owns clusters).
* **Decisions suggested, not taken:** dither or alpha blend between level and card (needs a per-instance field in the instance layout on
  every backend); live-fov / window-size dependent distances; 3D instead of ground-plane distance for a camera far above the field.
* **Cost to watch:** more straddling buckets near each boundary (ring about `from/factorHi .. from/factorLo`, 0.8..1.2 x for scales
  0.8..1.2; wider for wide-scale presets such as rocks 0.6..2.4). Measure `FoliageStraddlers` in a F9 capture on the real scene
  (baseline procedure `docs/auto-lod-baseline-2026-10-29.md`; remember `--scene` and an APFS clone).
* **Next steps:** 1. levelled witness / perf on the Foliage-Testszene; 2. step 6 cluster culling; 3. optional blend between level and card.
