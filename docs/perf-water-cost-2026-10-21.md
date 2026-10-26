# Water material close-up cost on Metal (Thema 235, step 1: profiling, no fix)

21.10.2026, MacBook Air M5 (10 cores, 24 GB), Metal, Release build of this branch (`12be6adc`). Raw data: `perf-audit/raw-water-235/` (`runs.json` = one slim record per run, 294 runs,
`conditions.txt`, `material-live-witness.txt`). Tools: `scripts/perf/gen_water_scene.py`, `water_matrix.py`, `water_table.py`; the two temporary patches are kept as
`scripts/perf/water_variants_mat_gen.patch` and `water_material_live_witness.patch` (applied only locally, never compiled into the tree). **No source file was changed.**

## 0. Result in seven lines

1. **The drop below 10 FPS does not reproduce.** Worst measured frame with the water graph live: GPU 17.8 ms (display locked) / 12.3 ms (display awake), at 2840x1528, camera 3 m over a 120 m lake. With the player's config
   (deferred, GI diffuse on; GI reflections on or off makes no difference) 19.2 ms for the whole GPU frame, 12.8 ms of it in the water's `Scene` pass. At twice the render scale (17 MP) the `Scene` pass is 18.8 ms.
2. **Every earlier "water is free" number measured the white fallback, not the water shader.** In this harness (and on any Mac with the EngineContentCache) `getMaterial({0x400,1})` is null while `isLoaded` is true: the UUID
   collision of Thema 230 (water material = `T_Landscape_Grass_Albedo`). Witness section 1. Thema 237's "water costs 0 ms" (its section 5/6 caveat) is consistent with this (almost certainly explained by it; its cells were not rerun with the witness). Everything in sections 2 to 4 was therefore measured with an **empty `HOME`** (no cache), where the graph is live.
3. **The cost is the water fragment shader, linear in covered pixels, and it is large only where the sheet fills the view.** Scene pass at scale 1 (4.3 MP): 2.4 ms without water, 11.4 ms with it (3 m up); 17 MP: 18.8 ms. At 130+ m the lake costs 0.0 ms, at 30 m about 8.6 ms (whole-frame GPU).
4. **Biggest single part: the four chop layers** (12 FBM reads per pixel): -2.8 ms (25 %) at scale 1, -7.1 ms (38 %) at scale 2 when removed. Then the six wave trains with hex bombing (~1.4 ms), the underside `If` (both branches are computed, ~0.95 ms), foam (~0.8), caustics (~0.3, noise level), shore readback (0.6 at scale 1, 0.0 at scale 2: noise level). Lighting and blend of the sheet itself are cheap (`lean` variant 3.0 ms vs 2.4 ms without water).
5. **CPU: nothing.** `WaterSurface::update` builds the mesh once (frame 3, 60 to 115 ms once, 5128 vertices) and is idle afterwards; the only CPU scope that changes with water is `Metal::NextDrawable`/`WaitForFrame` (present throttle of the locked display).
6. **OpenGL (one pair, display awake): +3.5 ms** (4.9 -> 8.4 ms, 205 -> 119 FPS), not worse than Metal.
7. **Warning for step 2:** once the 230 fix lands, the graph is live for everybody. That is +9 to +12 ms `Scene` at 4.3 MP on this M5. The perf work has to ship with, not after, the UUID fix.

## 1. Is the water shader live? (measured)

Temporary log in `WaterSurface.cpp` after `ensureWaterMaterial` (`getMaterial`/`isLoaded` of `kEngineWaterMaterialId`; `material-live-witness.txt`):

| Run | `getMaterial` | `isLoaded` |
|---|---|---|
| lake scene, Auto Landscape terrain, real `HOME` | **null** | 1 |
| same lake, plain terrain (no material), real `HOME` | **null** | 1 |
| plain terrain, `HOME=/tmp/he235/home` (empty) | non-null | 1 |

Asset A/B at scale 2, detailed, 3 reps (`live-det-s2-*`, real `HOME`): `Water.hasset` present vs deleted gives identical passes in all four cells (Scene 8.20 / 8.19 ms at near3 Auto Landscape; 5.68 / 5.64 at down3), i.e. the graph never ran.
With empty `HOME` the same view (plain terrain, near3, 60 frames, single pair) jumps from ~7 ms to 18.5 ms whole-frame GPU. The editor loads the cache textures whatever the scene is, so this holds for every machine that has synced EngineContent.

## 2. How it was measured

- **Scene:** `gen_water_scene.py`: 512 m landscape, parabolic basin of 120 m radius and 6 m depth around the origin, one body (id 1, level 0), `waterRes` 256, sky/weather from `docs/perf-audit/scenes/landscape_noclouds.hescene`,
  the control is the same terrain without the body. `--plain` drops the Auto Landscape material (cheaper terrain, no cache textures). `he_perf_capture.py --scene` opens it in the real editor loop, grid off, vsync off, 200 frames warm-up.
- **Cameras** (`x,y,z,yaw,pitch`; **yaw 0 looks along -Z** (`EditorCamera::forward`), the lake is looked at with yaw pi, my first run had it backwards and was thrown away): `far` 0,10,-250 (130 m to the shore), `mid` 0,10,-150 (30 m), `near3` 0,3,-60 (3 m over the water),
  `near1` 1.2 m, `down3` 3 m looking steeply down (water on every pixel), `under` 1.5 m under the surface.
- **Runs:** `water_matrix.py` (method of `autoland_matrix.py`: 3 repetitions, order forward / backward / forward, runs overlapping a foreign compile discarded and repeated, conditions file per run). 400 frames (`--no-counters` for whole-frame GPU, `--detailed` for exclusive pass times).
  Pass p50 over the frames, then the median over the three repetitions; the clock witness is Tonemap (0.30 to 0.37 ms in all water rows, 0.46 to 0.89 in the no-water rows: those frames run at a lower GPU clock, so no-water pass times are inflated, read them only as an upper bound).
- **Conditions** (every `conditions.txt`): AC power, `lowpowermode 0`, **screen locked** (`CGSSessionScreenIsLocked=Yes` in all runs), editor framebuffer 2840x1528 (the 3D viewport is a panel inside it; its size was not logged), no compiler/linker/second editor before or after a kept run.
  The locked display throttles presents (`NextDrawable` ~10 ms) and inflates whole-frame GPU time; later groups ran under `caffeinate -d -u` (lock stays, display awake; whole-frame 17.8 -> 12.3 ms, same cell, single pair). FPS is not read from locked runs.
- **Binaries:** `libHorizonScene.dylib` `0b9d965794f2` (with the temporary log line, used from the `live` groups on; the first groups `base`, `player`, `settings` ran on the same tree without it), `libHorizonRendering.dylib` `07fc5957615a`, editor `2e30aefadb62` (thin launcher, unchanged),
  `Water.hasset` shipped `934c2a7b1346`, `mat_gen` unpatched output `add37f179edc` (74 bytes different from the shipped asset; same Scene cost within 0.07 ms at scale 2, so the shipped asset is not stale in any way that matters here).

## 3. Whole frame, live graph, plain terrain (`base-live-*`, locked, scale 1, no counters, GPU p50 ms, 3 reps)

| Camera | no water | water | delta |
|---|---|---|---|
| far (130 m) | 8.04 | 7.91 | -0.1 |
| mid (30 m) | 8.04 | 16.68 | +8.6 |
| near3 | 7.72 | 17.84 | +10.1 |
| down3 | 8.15 | 17.18 | +9.0 |
| near1 / under (no control) | – | 17.45 / 17.55 | – |

Player config (`RenderPath 1`, GI on, occlusion culling on; detailed, display awake, scale 1; `player-live-det-*`): near3 whole GPU 9.06 -> 19.17 ms (Scene 0.39 -> 12.84, GI passes shrink because water covers pixels), down3 7.71 -> 16.34, far 9.50 -> 9.23.
With `GIReflectionsEnabled` on: near3 9.09 -> 19.43, down3 8.51 -> 16.90, far 9.35 -> 12.02 (SSAO 3.28 -> 4.22, Scene 0.45 -> 1.66; few water pixels there, not looked at further). **GI reflections do not change the water's own Scene cost** (12.84 vs 12.11 at near3).
Editor settings with the fallback water (default editor, `settings-*`, deferred / GI / SSR / TAA / SSAO off): all 7 to 11.5 ms, uninformative for the same reason as section 1.

## 4. Which part of the graph (`Scene` pass p50, detailed, display awake, plain terrain, near3, 3 reps; deltas against `shipped`)

| Variant (temporary `mat_gen` patch, same shader otherwise) | Scale 1 (4.3 MP) | Δ | Scale 2 (17 MP) | Δ |
|---|---|---|---|---|
| no water | 2.40 | | 5.65 (clock-inflated) | |
| **shipped graph** | **11.39** (11.31..11.42; second group 10.48..11.92) | | **18.81** (18.80..18.82) | |
| no chop (all 4 layers, 12 FBM) | 8.59 | -2.80 | 11.68 | -7.13 |
| no fine layers (r3 0.4 m, r4 0.17 m) | 9.98 | -1.41 | 14.33 | -4.48 |
| no finest layer (r4 only) | 10.46 | -0.93 | 17.06 | -1.75 |
| no foam | 10.60 | -0.79 | 16.45 | -2.36 |
| no underside branch (the `If` computes both) | 10.44 | -0.95 | 16.89 | -1.92 |
| no caustics | 11.10 | -0.29 | 17.39 | -1.42 |
| no shore readback | 10.77 | -0.62 (noise: runs 10.66..11.48) | 18.82 | +0.01 |
| unlit (lighting off, graph unchanged) | 10.76 | -0.4 | 17.18 | -1.6 |
| waves only (6 sine trains + hex bombing + warp FBM) | 4.34 | | 10.59 | |
| no waves, no chop | 3.96 | | 10.56 | |
| lean (no waves, chop, caustics, foam, underside) | 2.98 | | 8.71 | |
| asset deleted (white fallback) | 2.18 | | 5.57 | |

Reading: the parts are far from additive (removing chop alone saves 2.8 ms, removing everything saves 8.4 ms at scale 1; caustics + foam + underside cost 4.3 ms with the wave trains in (`nochop` 8.59 -> `waves-only` 4.34) but ~1.0 ms with them out (`nowaves` 3.96 -> `lean` 2.98), while their single toggles sum to ~2.0). That points at occupancy / register pressure of the big shader, not pure ALU: check it before assuming that fewer FBM reads are a linear win. The deltas against no water are lower bounds (those rows ran at a lower GPU clock); the sheet itself (lighting, blend, shore tint) is ~0.6 ms at scale 1. The `trivial` variant (flat colour, **not** in the table) was not trivial: the underside's emissive still pulls the whole wave chain in, so it cost nearly as much as the shipped graph (10.4 vs 11.2 ms at scale 1, 14.5 vs 18.7 at scale 2).
ShallowWater is a parameter (`Vec4`), not a pass: its cost is inside "shore/tint", at most 0.6 ms.

## 5. What differs from the reported case (the drop does not reproduce)

- The reporter's water is, with the cache present, the **white fallback** (cheap) on Metal; so either they had the graph live (a build with the 230 fix, a pak without the texture) or the slow thing is not the water graph.
- Reporter: probably the game (`HorizonGame`, 1280x720 fullscreen or native, vsync on), maybe `GameBackend: OpenGL` as in the Catania export config (one OpenGL pair only here: +3.5 ms), the real Catania world (2000 m, 11 bodies, clouds, 25 ms before any water), thermal state of a fanless Air, low-power mode, battery.
- Needed for <10 FPS is ~100 ms; the worst here is 19 to 22 ms, so something else multiplies the cost five-fold.

## 6. Not measured

Whether the exported `HorizonGame` loads the EngineContentCache at all (237: the HOME-based cache supplies arrays when the deploy has none; the pak path may differ), ripple rings (the `Water Ripple` loop runs up to 16 rings once a body splashes; every run here had 0 rings), the Catania scene with the live graph, `HorizonGame`, battery / low-power / thermal throttling, a moving camera, several overlapping bodies (overdraw), D3D11/D3D12/Vulkan (not verifiable on this Mac), OpenGL beyond one pair, the 3D viewport's pixel count,
Thema 234 (puddle flicker; no link found, none excluded), the editor's own UI cost (CPU p50 10 to 14 ms in every cell, unchanged by water).

## Handoff

**Done (verified by the runs above):** liveness witness of the water material (fallback in the harness), per-distance and per-camera cost with the live graph, player config incl. GI and GI reflections, graph toggles at two render scales (3 reps each), CPU scopes, one OpenGL pair.
Commit: this doc, `scripts/perf/{gen_water_scene,water_matrix,water_table}.py`, the two `.patch` files, `perf-audit/raw-water-235/`.
**Not verified:** section 6. The <10 FPS report itself.
**Decisions taken:** empty `HOME` + plain terrain for all live-graph numbers; Scene pass at scale 1 and 2 instead of whole-frame GPU (locked display); toggles by regenerating `Water.hasset` from a temporary `mat_gen` patch (sources reverted, deploy restored to the shipped asset). **Only suggested:** the levers below.

**Next steps, in order (step 2):**
0. Ask the reporter ONE question first: when the FPS drops, is the water WHITE or does it show waves/foam? White -> the graph is not running for them (UUID collision), look elsewhere (section 5); shaded -> the graph is live for them (which build/pak?) and the table in section 4 is the fix list.
1. Tell the queen: the 230 fix turns the graph on for everyone (+9 to +12 ms Scene at 4.3 MP). Do not merge it without a cost cut, or merge both together.
2. Reproduce the reporter first: ask which build/backend/scene/resolution; run `water_matrix.py base --home <empty> --plain` against their numbers. If their cost is 5x ours, force 16 ripple rings (temporary fill of `WaterRippleBlock` in the extractor) and run the Catania scene with the live graph (copy `/tmp/he237/scenes`, empty `HOME`).
3. Levers by measured size: (a) chop: one gradient-noise read per layer instead of three FBM reads (12 -> 4 reads, up to -2/3 of 2.8 / 7.1 ms), or fewer layers/octaves in the mid field; (b) a real branch on `below` (a camera-level uniform) instead of `If` computing both sides (-0.95 / -1.9 ms); (c) near/far material variants by distance so the 30 m+ sheet runs the `lean` graph (cost 3.0 vs 11.4 ms); (d) foam FBMs only where `crest` is high.
4. Re-measure with the same scripts (`water_matrix.py variants --home <empty> --plain --detailed [--scale 2] --caffeinate`), same cells, compare p50 and the Tonemap clock witness; then the other backends (Windows machine for D3D/Vulkan, GL here).

**Pitfalls:** `--tag -x` is parsed as an option (use `--tag=-x`); `ioreg` and `screencapture` are not on PATH (`/usr/sbin`), and a locked screen gives no screenshots (`screencapture` fails); a variant named `none` collided with the driver's "delete the asset" case, it is `noasset` now;
`mat_gen` with `HE_MG_VARIANT` skips its parameter check (patch only); deploy restore: `cp /tmp/he235/Water.hasset.orig .../EngineContent/Materials/Water.hasset` (`water_matrix.py` does it in `finally`).
