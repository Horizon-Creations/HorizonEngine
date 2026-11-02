# Auto Landscape 2.0: layer meshes on the instancing path (Thema 180, Schritt 5)

Branch `claude/auto-landscape-material-2-0-alle-backends-bcn-instancing-audit`, 21.10.2026. Builds on `docs/auto-landscape-material-2-layer-meshes-2026-10-21.md`
(the meshes and presets, step 4) and `docs/foliage-instancing-lod-plan-2026-10-10.md` (clusters, levels, arena, Thema 163/182).

## 0. Result

**No backend code was needed.** A layer mesh is an ordinary `FoliageLayer` with `lods`, so it already travels as one cluster `RenderObject` per bucket and
level (`FoliageExtract.cpp`), unfolded to instances only by `GeometryPass` and `RenderSorter::batchDepthRuns`; all five backends draw clusters since Thema 182
(plan §11.4 Metal, §16.2 GL/D3D11/Vulkan, §17 D3D12 arena). What was missing was **proof for the six real preset meshes** and two traps that only show with
engine *files*. This step adds the witness, the unit tests, a CI case for Vulkan (lavapipe) and the two D3D backends, and the measurements below.

## 1. What the step added

| What | Where |
|---|---|
| Witness `HE_DUMP_AUTOLANDMESHES=tuft,pebble,rocka,rockb,rockc,snowdrift` on the Auto Landscape witness relief; `AUTOLANDMESHDENS=<f>` (density multiplier), `AUTOLANDMESHSHADOW=0/1` (no layer / every layer casts), `AUTOLANDMESHRULES=0` (placement rules off) | `EditorApplication.cpp` (before the `HE_DUMP_AUTOLANDMIRROR` block) |
| 6 unit cases | `tests/test_layer_mesh_instancing.cpp` (in `tests/CMakeLists.txt`) |
| CI case `layer_meshes` (+ `--rhi`, `flat` verdict, draw/triangle counters) | `scripts/he_vk_imagetests.py`; runs in the `vulkan-lavapipe` job by default |
| Windows step "Layer-mesh image test (D3D11, D3D12)" (informational, `continue-on-error`) | `.github/workflows/ci.yml`, after "Run tests" |

The witness loads every level file (`layerMeshLevelPath`) and logs `16 of 16 level mesh(es) loaded with their engine UUID`: engine files are **not resident by
themselves**, and the extractor reads a cluster's box from the ContentManager. A mesh it cannot read gets an invalid box, which means *never culled*
(`RenderObject.h` cluster note). The Inspector's preset button loads the files, a reopened scene gets them from `SceneSystems::preloadAssetRefs`
(`collectAssetRefs` names every level), a headless dump needs the witness to do it. Both ways are pinned by tests.

The presets assume a terrain at y=0 (Snow Drift `heightMin 66` is world Y). The witness terrain sits at y=300 with its snow line at 320, so it moves the
drift's `heightMin` to `snowHeight + 6`.

## 2. Draw count against instance count

Metal and OpenGL, headless dump (`he_shot.py`, `AUTOLAND=1` shipped material, `SKYTEST=1 TOD=0.4 CLOUDMODE=0 COVERAGE=0 AA=0 RENDERPATH=0`), camera `CAMX=-60 CAMY=312 CAMZ=0 PITCH=-20`.
`dump counters` is the renderer's own `draws=` after culling, batching and (on GL/Metal) shadow cascades not included.

| density | instances in the store | instances emitted (in range) | clusters | draws Metal | draws GL | triangles Metal = GL |
|---|---|---|---|---|---|---|
| no meshes | 0 | 0 | 0 | 38 | 38 | 330 752 |
| x1 | 6 653 | 1 408 | 53 | 47 | 47 | 381 624 |
| x4 | 26 732 | 5 669 | 66 | 49 | 49 | 538 016 |
| x16 | 106 891 | 22 775 | 72 | 49 | 49 | 1 148 992 |
| x64 (Metal) | 427 631 | 91 691 | 72 | 49 | n/a | 3 679 316 |

So the foliage adds **9 to 11 draws** (distinct mesh and level in play, 16 meshes at most), whether it carries 1 400 or 91 700 plants. x1 shows two fewer draws than x4/x16
(my reading, not checked mesh by mesh: a rare level, e.g. of the rocks with 13 to 17 plants each, is absent at that density). Top-down view (`CAMX=-20 CAMY=345 PITCH=-89`, default terrain material): 9 / 20 / 20 draws for
off / x4 / x16, triangles 78 336 / 436 732 / 1 534 532, **identical on Metal and OpenGL**.

The same scene with `HE_FOLIAGE_CLUSTERS=0` (one RenderObject per plant) shows **draws 45** (also batched) but `visible=469/1673` objects against `55/318` with clusters: the
draw count does not tell the two paths apart, the *object* count does. At x64 the cluster path hands the passes 337 objects for 427 631 scattered plants.

The 65 536 cap (`kMaxInstancesPerDraw`) is **not exercised** by these numbers (the largest single mesh run at x64 is far below it); `GeometryPass` cutting at the cap is held by
`test_foliage_cluster.cpp` ("GeometryPass cuts a run longer than one instanced draw may carry").

Unit form of the same claim (`test_layer_mesh_instancing.cpp`, "four times the plants draw in the same number of calls"): the real cull and sort, then `GeometryPass` and
`batchDepthRuns`: x1 and x4 carry the same meshes in the same number of scene draws (at most 20) and the same number of shadow and AO draws. **The test caught the trap that
the draw count is a count of runs after the sort**: handed the extractor's unsorted order the same scene gave 21 against 26 draws.

## 3. Passes and culling

* **Deferred and forward** use the same `GeometryPass` run (unit test: scene draws carry every kind; Metal/GL images above).
* **Shadow and AO**: the preset flags decide (rocks cast and contribute, tufts, pebbles and drifts do not). Unit test: rocks are in `batchDepthRuns` for shadow casters and AO
  contributors, tufts and pebbles are not, shadow meshes are a subset of the scene's, shadow plants are fewer. A rock's `shadowDistance` (60 m) is shorter than its AO reach, so shadow
  instances are at most the AO instances.
  * OpenGL pixel witness: rocks on the ramp with shadows on against off (`AUTOLANDMESHSHADOW`), mean difference 20.3 of 255, the rocks shade each other (attached image).
  * Metal: the log says `MetalRenderer: shadow pass instanced (first run: 260 casters)` with the flag on and no such line with it off, so the cluster casters reach the instanced depth
    pass. **I did not get a pixel difference on Metal** (0.001 mean, in this scene and with the default terrain material); the Metal shadow pass itself is healthy (the
    `SHADOWINSTTEST` cube row shows its shadows), so the likely cause is the witness lighting, not the instancing. Open, see section 6.
* **Culling uses real boxes**: all clusters of the six kinds have a valid `worldBounds` when the files are resident, and all are invalid when they are not (test, with that control).
  The frustum cull drops buckets (kept < all) and a camera turned away keeps fewer.
* **Levels**: Tuft shows all three meshes from a rim camera. Hysteresis per view is `test_foliage_lod.cpp` (held plants exactly the band; a second view has no memory); a
  single-frame dump starts with an empty memory, so `held` is 0 there by construction.

## 4. FrameKey

**No new input.** A layer's rules, levels and scatter are component values the walk reads; the frame reuse's contract is an unchanged world between two extracts of one frame
(`RenderExtractor.h`, `beginFrame`), and component values were never in the key. The cluster path adds one state of its own, `FoliageLodMemory`, which lives on the extractor, i.e. per view. Test
"one walk per frame, and the next frame sees a changed rule": `full == 1`, `reused == 1`, objects found in place, the same clusters and instances; after an edit and a re-scatter
the next frame walks again (`full == 2`) and carries fewer plants; a second extractor (a second view) chooses the same plants from the same camera.

One honest gap, not a bug: mesh residency is not in the key either. A mesh that becomes resident *between two extracts of one frame* keeps an invalid box for the rest of that frame
(clusters are never refined) and is boxed from the next frame on. Nothing loads meshes mid-frame today.

## 5. CI

* `vulkan-lavapipe` runs the new case `layer_meshes` (it runs every case by default): three shots (off, x4, x16, default terrain material, top-down). Verdicts: off against x4 differs by at least
  0.8 mean (measured 1.7 Metal, 2.1 GL: half), the log has `6 layer(s), 16 of 16 level mesh(es) loaded with their engine UUID` and the `FoliageExtract: 6 layer(s)` line, none says `cluster(s) for 0 of`,
  and `flat x4 -> x16`: draws grow by at most 2 while triangles grow. Run locally on Metal and OpenGL through `--rhi` (draws 2 / 13 / 13, triangles 17 408 / 375 804 / 1 473 604, same on both);
  negative controls (tolerance -20, min 9.0, a wrong required string) all go red.
* The Windows job (NN-WS03 hardware when the Hive routes it there, WARP on a hosted runner) runs the same case with `--rhi D3D11` and `--rhi D3D12`, informational. **Not seen on any Windows machine.**
  Windows CI also runs the new he_tests file (CPU only).
* Run IDs: see the result post (CI is not awaited).

## 6. Not verified

D3D11, D3D12 and Vulkan have not drawn a layer mesh by me: no code changed there, their cluster path has been shown on hardware by Thema 182 (plan §16-17) with a cube and a sphere, and the new CI case is
the first run with the layer meshes. Hardware smoke on NN-WS03 stays open. No Metal pixel witness for rock shadows. The meshes have no material (default white; plan §8.5, not this step). Draw *time* is step 6.

## Handoff

- **Topic / step:** 180, step 5 of 10
- **Done:** layer meshes shown on the cluster path on Metal and OpenGL (images in the topic files), draw counts flat (table §2), witness, 6 unit cases, CI case + Windows step, FrameKey decision (no change).
- **Files:** `EditorApplication.cpp` (witness, before `HE_DUMP_AUTOLANDMIRROR`), `tests/test_layer_mesh_instancing.cpp`, `tests/CMakeLists.txt`, `scripts/he_vk_imagetests.py` (case `layer_meshes`, `--rhi`, `flat`), `.github/workflows/ci.yml` (Windows step).
- **Verified:** he_tests `*test_layer_mesh_instancing.cpp` 6/6 (macOS release); Metal and GL dumps and counters as in §2; `he_vk_imagetests.py --rhi Metal|OpenGL --cases layer_meshes` passes and its negative controls fail; `editor_help_audit.py --check` ok.
- **Not verified:** D3D11/D3D12/Vulkan (CI judges), Metal rock-shadow pixels, the Windows step, the 65 536 cap with real meshes, draw time.
- **Decisions taken:** zero backend change (the path is complete); CI case on the default terrain material (cheap, no texture arrays); Windows step informational; no FrameKey field.
- **Decisions suggested, not taken:** a surface material for the meshes (plan §8.5); make the Windows step blocking once it has run green on NN-WS03; a Metal rock-shadow witness on the `SHADOWINSTTEST` floor.
- **Traps:** engine mesh files are not resident until loaded (invalid box = never culled; witness and presets load them); the draw count is a count of runs *after* `RenderSorter::sort` (unsorted input inflates it); `HE_DUMP_SHADOW=0.1,...` does not switch the shadow pass off (use `AUTOLANDMESHSHADOW=0`); `he_shot.py` and `he_vk_imagetests.py` give different absolute draw counts for one scene (different defaults), compare within one tool; the Metal/GL picture differs by ~5 mean on this scene before any mesh (pre-existing); D3D12's `draws=` includes shadow cascades.
- **Next steps:** 1. read the CI results (`vulkan-lavapipe` case `layer_meshes`, Windows step artifact `layer-mesh-shots-windows`); 2. step 6 (audit: pass p50 with layer meshes on/off, `he_perf_capture.py`); 3. the meshes' material.
