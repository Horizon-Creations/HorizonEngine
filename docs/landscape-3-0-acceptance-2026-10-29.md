# Landscape 3.0: acceptance (Thema 233, step 5), 2026-10-29

Branch `claude/landscape-3-0-textur-layer-gruppen-forest-grassland-mountain` @ `cedfd65c` (+ this doc and `scripts/landscape-3-0-accept/cap233.sh`).
Mac (M-series, macOS), Release, Metal and OpenGL. Nothing in `src/` was changed by this step.

## Verdict per backend

| Backend | Verdict | How |
|---|---|---|
| Metal | **verified** (visual, tests); perf **not measured** | build, ctest, Mountains pixel A/B, group witnesses, mesh witnesses. Perf: screen locked and machine under load, so absolute pass-p50 is unusable; the Scene/Bloom proxy (below) shows no shift, it is not the asked-for p50 A/B |
| OpenGL (macOS 4.1) | **verified** | same witnesses, headless dumps; perf not measured (no per-pass counters run) |
| Vulkan | **not verified** | SDL fails here: "Failed to load Vulkan Portability library", no MoltenVK. `git diff 31d6253c..HEAD -- src/HE_Rendering/src/Backends` is empty (no backend code changed); the cross-compile/bake sweep in `test_landscape_groups`/`test_material_graph` is green |
| D3D11 / D3D12 | **not verified** | NN-WS03 offline (`hive_devices`: last seen 3h ago). Same: no backend code changed, cross-compile green. Hardware step stays open |

## Results

* **Build/tests:** `HorizonEditor`, `he_tests`, `landscape_tex_gen`, `mesh_gen` up to date, rc 0. Full `ctest -j4` in `out/build/release` with an empty
  `HOME`: **344/344 passed**, 194 s. `editor_help_audit.py --check`: 1244/1244.
* **Mountains is pixel-identical to the pre-3.0 build.** Baseline = editor built from the merge base `31d6253c` in a scratch worktree (proved by
  `strings`: only the new one has `HE_DUMP_AUTOLANDGROUP`). Via `cap233.sh` (GI/SSAO/AA/bloom off, HE_SKY_TIME pinned, private HOME), md5 of the BMP, old vs new:
  Metal real arrays `5c591e66`, placeholder arrays `48ddc363`, layer meshes `7848209f`; OpenGL `ac06582d`, `ddda10ce`, `ebcffdf2`. All six equal. Two runs of one
  binary are equal too (noise floor 0).
* **Forest / Grassland differ and draw their own tones** (Metal `aeb8a52f`, `f2c4d613`; GL `aaa744d9`, `8bc5e795`; `draws=4`, no "Cannot load asset"). Looked at the
  frames: Forest brown/olive with more dirt, Grassland light green with little dirt, Mountains green/brown (all 128^2 placeholder arrays, same relief). Metal and GL agree.
* **Per-group meshes** (`HE_DUMP_AUTOLANDMESHES`, density x4, camera `-30,306,0` pitch -50): Forest 4 layers / 10 of 10 level meshes loaded (Fern 5086, Stump 81,
  Log 51, Moss Rock 86), Grassland 5 / 13 of 13 (Tall Grass 21192, Flower Tuft 3185, Pebble 10694, Rock A 49, Scree Chip 577), Mountains 6 / 16 of 16. Counters
  are identical on Metal and GL. Stumps, logs, moss rocks, grass, pebbles look the same on both backends.
* **Performance (Metal, `landscape_regression.py --detailed`, 128 m relief, 3 reps alternating, 2840x1528):** the design doc states **no absolute budget**
  (its only claim: nothing group-specific reaches a backend, 13/16 params), so the criterion used is "Forest/Grassland within noise of Mountains with the same
  array size". Absolute GPU p50 is NOT usable: the same cell ranged 8.9 to 14.4 ms between reps (screen **locked** in all 30 conditions files, CLion ~110% CPU, GPU
  clock state). Every pass moves together, so Scene/Bloom per run is the stable figure: base Mountains (2K real) 9.91, new Mountains 10.13, Mountains ph 9.94,
  Forest ph 9.81, Grassland ph 9.68 (spread within a cell <= 1.14x). No difference beyond noise. Raw: `/tmp/l3perf/raw` (not kept). Needs a quiet, unlocked M5 for numbers.

## Findings (not fixed, no regression in the step's scope)

1. **Fern looks different on Metal and OpenGL** (`docs/landscape-3-0-acceptance/fern-metal.png`, `fern-opengl.png`, same camera, shadows off): Metal grey-blue with
   bright stripes, GL almost white with a dark wedge at the base. The other six new kinds and the Mountains tuft (same strip construction) agree. Not diagnosed;
   suspect the fern's strip normals (`makeBunch` with `drop` 0.45 in `MeshGen/main.cpp`) lit differently per backend. Fern also reads as a spiky star at distance.
2. Forest is 96% fern at x4 (log/moss rock are rare); density per kind is a tuning item, not a bug.
3. Mountains and Forest/Grassland look different only because of the arrays; a terrain bound to a group without its arrays draws the white hexagons.

## Handoff

* **Verified:** table above; scripts reproducible: `scripts/landscape-3-0-accept/cap233.sh OUT ARRAYDIR GROUP RHI [TAG [HE_DUMP_X=..]]`, `ED=<editor dir>` for the baseline.
  Placeholder arrays: `landscape_tex_gen <dir> --group Forest|Grassland` (Mountains: no `--group`).
* **Not verified:** Vulkan/D3D11/D3D12 at runtime; GL performance; absolute GPU times; live editor GUI (group picker, Apply Group Meshes, only the headless UI tests drove
  them); pak export of a Forest project end to end; a scene saved before groups loading as Mountains in a running editor (unit-tested only).
* **Open for the human:** (1) upload Forest/Grassland arrays (+ 18 mesh files, ids 0x119..0x12A) to EngineContent, otherwise white hexagons / missing meshes; (2) shipping V
  and size (832 MiB per group at 2K, V=3, design §5.2); (3) texture source (§5.3); (4) mixed groups in one terrain (§5.1, option C); (5) fern look, see finding 1.
* **Traps:** `test_hpak` "scanContentDirectory == 2" is red with a populated real EngineContentCache: run ctest with `HOME=<empty dir>`. A stale `/tmp/al2/projA` has no
  `.heproj`. The scratch baseline worktree `/tmp/l3base` was removed after the run.
* **Next:** run the same on NN-WS03 when it is back (D3D11/D3D12/Vulkan: `frame_pixel_ab_win.ps1`, the `cap233.sh` pinning by hand); re-measure perf unlocked.
