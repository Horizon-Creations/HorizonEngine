# Landscape 3.0 Folge (Thema 329): overall report and handoff, steps 1-7

Branch `claude/landscape-3-0-folge-path-layer-prozeduraler-schnee-sand-mate`, head 9765b2d3 + this doc. Read the per-step doc
before touching its area; this page only ties them together.

## What the topic delivers

| Step | Result | Doc |
|---|---|---|
| 1 | Third weight page: 12 paintable layers (page count = weightmap width/height, 1:1, 2:1, 3:1). Old scenes load unchanged. Collab protocol 17 -> 18. | `landscape-3-0-weight-page-3-2026-10-29.md` |
| 2 | `MF_ProceduralSnow` / `MF_ProceduralSand` (ids 0x41D / 0x41E, `Surface/`), weather-driven; Snow role (all groups) and Desert ground use them. No new landscape parameters (count stays 13). | `landscape-3-0-procedural-snow-sand-2026-10-29.md` |
| 3 | Path layer: paint-only channel 8, standalone Albedo texture per group, Dirt's normal slot; channels 6/7 gap kept. | `landscape-3-0-path-layer-2026-10-29.md` |
| 4 | Water tool paints river-bed pebbles on a per-group Water layer (channel 9, reads the WetGround slice); brush `pebbleLayer`/`eraseLayer`. | `landscape-3-0-water-pebbles-2026-10-29.md` |
| 5 | Mountains rock by size: variants 0 medium / 1 large / 2 small by slope; Mountains only, other groups byte-identical. About +3 ms Metal scene cost. | `landscape-3-0-rock-by-size-2026-10-30.md` |
| 6 | **Open**, see below. | this doc |
| 7 | CI regression of step 2 fixed (argument-evaluation order in `ProceduralSurfaceFunctions.cpp`, d27fad34). | step 2 doc, "Step 7" |

## Step 6 status (honest)

* **Done:** acceptance on the Mac at the branch head: `he_tests` built, full `ctest -j4` **345/345** (run with a private `HOME`,
  otherwise `test_hpak` counts the real EngineContent cache). CI on this branch is green after the step-7 fix (runs 40942374987
  and 40962329081); run 40977874461 for the head was still running when this was written. Trial merge with topic 327
  (`neuer-asset-typ-landscapefoliagelayer...`): `git merge-tree` reports **no textual conflicts**, although both touch
  `MaterialGraph.cpp/.h`, `TerrainTools.cpp`, `EditorApplication.cpp`, `EditorHelp.cpp`, `SceneSerializer.cpp`,
  `test_material_graph.cpp`, `test_terrain_tools_ui.cpp`; the merged result was **not built**.
* **Not done: packing real textures.** The picker assignment (the human's result text) does not exist anywhere reachable: no Hive
  post, no file in the worktree or `~/Downloads` (42 Poly Haven zips are there, but nothing says which belongs to which group
  role). I did not guess a mapping, as it is the human's choice. Nothing in `EditorDeps/EngineContent/Textures/Landscape` was changed.
* **Not done: D3D11/D3D12/Vulkan check.** NN-WS03 was offline (last seen 1h before this run), and nothing was run on the Mac for those
  backends. They stay open (steps 1, 2 and 5 list them as not verified; step 3's tests ran on NN-WS03, a test run, not a D3D/Vulkan render check).

## How to finish step 6 once the human delivers

1. Per group, stage the zips: `python3 -I scripts/landscape-textures/stage_polyhaven.py <source-root> <staging-dir> --size 2048`
   (throw-away venv with numpy, Pillow, OpenEXR).
2. Forest/Grassland/Desert: `landscape_tex_gen EditorDeps/EngineContent/Textures/Landscape --pack <png-dir> --group <G> --size 2048`
   with `<RoleLabel>_<Map>.png` (see `landscape-3-0-pack-real-textures-2026-10-29.md`).
3. Mountains: `Rock_<Map>.png` = rocks_ground_08 (medium), `Rock_v2` = 06 (large), `Rock_v3` = 01 (small); Water/WetGround slice
   carries the pebbles (step 4), Path has its own standalone texture per group (step 3, `writePathTextures` placeholders).
4. `--material [--group G]` only if a graph changed; the shipped-hasset tripwire in `test_material_graph` tells.
5. Re-shoot on Metal/GL (`scripts/he_shot.py`, recipes in the step docs), tune Boulder Slope / tile sizes, publish via the editor's
   "Publish Engine Content to Server". Season textures belong to Thema 330, only summer here.

## Open decisions and traps

* Collab protocol bump 17 -> 18 (step 1) needs a queen decision; release/0.8.0 peers on 17 drop the third page.
* Channel layout is load-bearing for Thema 330: 6/7 gap, 8 = Path, 9 = Water. Do not renumber `kAutoLandscapePaintLayerNames`.
* Any change to a function or to `emitLayerBlendNode` means `matfn_gen` AND `landscape_tex_gen --material` for all four groups.
* Builder code in `ProceduralSurfaceFunctions.cpp`: never two node-creating calls as siblings of one call (Hive lesson 602).
* GI bounce kernels still read layers 0..3 of the weightmap only (pre-existing, not changed here).
* Perf: step 5 adds about +3 ms Metal scene cost on the steep Mountains view; step 2's perf A/B is not done.

## Handoff

* **Done:** steps 1-5 and 7 committed and pushed; Mac acceptance 345/345; CI green after the step-7 fix.
* **Pending:** real texture packing (blocked on the human's assignment), D3D11/D3D12/Vulkan visual check (NN-WS03), EngineContent
  upload of the new functions, materials and textures, perf A/B, protocol decision, build of the 327 merge.
* **Decisions taken:** none new; step 6 left open rather than guessing a texture mapping.
* **Next:** 1. human sends the picker result, 2. run the "how to finish" list above, 3. NN-WS03 online: `HE_DUMP_AUTOLANDGROUP=<G>`
   witnesses on D3D11/D3D12/Vulkan, 4. merge into release/0.8.0 after 327.
