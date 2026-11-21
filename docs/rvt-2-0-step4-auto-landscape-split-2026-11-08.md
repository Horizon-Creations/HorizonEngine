# RVT 2.0 step 4: the auto landscape material cut at Write / Read (Thema 335, 2026-11-08)

`buildAutoLandscapeGraph(Lit, ..., displacement=false)` is now two programs (`AutoLandscapeMaterial.cpp`, `kAutoLandscapeRvtName = "Landscape"`):

| Before the RVT Write (pages, painted once) | After the RVT Read (live, per pixel) |
|---|---|
| layer blend of the groups (mix material too), texture bombing, dirt patches / dirt belt, rock-on-slope ramp ("Hang-Blend"), the painted Grass/Dirt/Rock layers (`wA`) | procedural snow (`MF_ProceduralSnow`, height + slope snow, `MF_WeatherSnow`, painted Snow share), the procedural sand ripples (`MF_ProceduralSand`), puddles and wet rim (`MF_WeatherPuddles`), trails (Thema 328 clipmap), weather |

Write pins: Base Color, Normal (normalized), Roughness, AO. Height and Mask stay unconnected (a Read does not give them back, the pool has three layers). Debug views (`MasksRockSnowWater`, `MasksDirtWet`, `Normal`, `Surface`, `BaseColor`) and the displacement variant keep the whole graph in one program. "Lichtung" has no counterpart in the file; nothing was invented for it.

## Decisions (the queen agreed to the sand one)
* Snow bias against the ground's relief: after Read the ground height is unknown, the ground counts as 0.5, only the snow's own height bends the edge. Puddle water-level bias (+-0.02) drops after Read.
* Painted Snow layer: the page treats it as ground; live, snow lies over the Read by `paintedS + autoShare * snowAny`. Automatic snow now also covers painted Grass/Dirt/Rock partially (before: only the automatic share).
* Desert and mix: the page bakes a plain sand colour (no ripple, roughness 0.85, terrain normal); live, `MF_ProceduralSand` is blended over the Read by the sand share = painted grass + automatic share x (1-dirt) x (1-rock), recomputed from slope + dirt fbm + weights, without the layers' height bias (a thin band at transitions can differ). Mix: times the Desert weight of the pair.
* `generateFragment` with RVT on gives the shipped asset the parameters, switches and texture slots of the page graphs too (declared, never sampled): the instance panel, instances' overrides, the GI (`giAutoLandscapeParams` finds the albedo array through `graphTexturePaths`) and the asset scan keep working. Shipped params are 15/16 + 2 (the Read's `RVT Landscape Volume A/B`).
* The consumer the driver draws is `split.live` alone: one texture (trail), no array.

## Driver (`RvtLandscape.cpp`, `LandscapeHlodBaker.cpp`)
* `HlodBaker::begin(..., rvtPages=true)`: the three view materials are cut from the material's own Write pages (`matRvtLayerViews` in `RvtSplit.cpp`: colour; normal*0.5+0.5; AO+roughness merged). `rebuildRvtViews()` rebuilds them in place (`replaceMaterial`, same ids; the renderer rebuilds a shader only when its text changed).
* Consumer = live graph with this landscape's parameters; the Read's volume vectors are written into its parameter block (`refreshConsumer`). A change after the Read: `liveOnlyChanges`, consumer refreshed, no page, no re-attach.
* A change before the Write with the same landscape: `pageRebuilds`, views rebuilt in place, `invalidateVolume`, resident pages repaint. No re-attach (before: scratch copy + volume rebuilt).
* Fallback: a landscape with RVT nodes is drawn by an inlined whole-material copy (`seg.fallback`) out of range, before the first fill and while paused (a Read of an empty pool would draw black). The pool is made for the first such landscape even if none is near.
* The pool has ONE stale flag per page: a changed roughness page repaints the colour layer too. Per-layer repaint is not done (`MatRvtMaterialState::pageHashes` already tells which layer changed; the manager would need a per-layer flag).

## Verified (Metal, this Mac, release)
* `he_tests`: `test_rvt_auto_landscape` (5 new cases: split of every group + mix, no frozen/weather/function in pages, no array tap live, snow/sand/puddle/look params leave every page hash alone, layer params change it, Bombing switch is a page input, inline vs live shader, debug views) and `*test_rvt_*`, `*test_material_graph*`, `*landscape*` green. Existing whole-material tests run under `tests/RvtOffScope.h` (RVT off = the inlined whole material).
* Driver witness `HE_DUMP_AUTOLAND=1 HE_DUMP_RVTDRIVER=1 HE_RVT=1 HE_DUMP_RHI=Metal HE_HIDDEN_WINDOW=1 HorizonEditor` (private HOME): pages + live consumer vs the whole material: mean 1.07/255, max 88 (forward 1.05, deferred 1.07); snow line change: re-attached NO, 0 page rebuilds, 0 stale, 0 painted, 1 live-only change; rock slope change: re-attached NO, 1 page rebuild, 21 pages stale, painted as the camera needs them; sculpt/paint strokes unchanged (7 and 6 pages).
* Live part cost, counted in the generated shader (test message): texture reads whole material -> live part: Mountains/Forest/Grassland 70 -> 10, Desert 52 -> 10, Mix 125 -> 11; GLSL 40.6 kB -> 23.0 kB (Mountains), 64.9 kB -> 32.7 kB (Mix).
* GPU time on Metal (`HE_DUMP_RVTDRIVER_FRAMES=150`, with and without `HE_DUMP_RVTDRIVER_DIRECT=1`, 3 runs each, 128 m landscape at 1280x720 from 110 m above): GPU frame 2.06 / 2.99 / 3.59 ms (pages + live) against 2.50 / 2.73 / 3.15 ms (whole material). The scene is too light and the noise larger than the effect: **no per-pixel GPU gain can be claimed from it.** A real number needs the editor profiler on a project with a big view (`scripts/he_perf_capture.py`, pass p50, RVT on / off).

## Not verified
* D3D11/D3D12/Vulkan/OpenGL (no hardware here). The pool pack of the normal layer and the Read lookup were already tested in step 333; the consumer's G-buffer tail ran on Metal deferred only.
* The shipped `.hasset` files carry the RVT-on shader. Packed exports: the cook follows the switch (step 2); not run here.
* Real arrays (the dump uses the placeholder arrays).

## Handoff
Next steps, in order: (1) per-layer stale flags in `rvt::RvtManager` so a changed Roughness/AO/Normal page repaints only that layer; (2) a profiler capture of the live part on a big view (`he_perf_capture.py`, RVT 1 vs 0); (3) D3D/Vulkan/GL run of `HE_DUMP_RVTDRIVER` on Windows; (4) debug map viewport mode (step 5+).
Traps: `ninja HorizonEditor` fails with "dev MISMATCH" when `out/build/release/sdk-release` was configured at another commit (its `HE_GIT_COMMIT` differs): `cmake out/build/release/sdk-release` fixes it. `timeout` does not exist on macOS. Landscape tests that read the whole material in one shader must run under `HE_TEST::RvtOffScope`.
