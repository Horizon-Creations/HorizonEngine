# Landscape 3.0 Folge — Step 4: the Water tool paints pebbles (Water layer per group)

## What this step adds

* A paint-only layer **"Water"** at weight channel **9** in all four groups' auto-landscape
  materials (`kAutoLandscapePaintLayerNames` ends `...\n\n\nPath\nWater`, `AutoLandscapePaintLayer::Water`,
  count 10). No further gap was needed after Path (step 3's reservation).
* **No texture of its own.** The graph is at `kMatMaxGraphTextures` (4: three arrays + Path's albedo), so
  Water reads the arrays' **WetGround slice (4)**, the one slice nothing read before, bombed like Grass/Dirt/Rock
  (seed 53, normal + mask from the same slice). A group's pebble photograph is therefore *packed into the
  WetGround slice* of that group's three arrays (step 6). Until then it shows the shared dark wet-ground
  placeholder (`landscape_tex_gen` kLayers `WetGround`, never retoned per group).
* Weight extraction: a third `LandscapeLayerBlend` (`wC`, Water alone, red channel); the three mix blends and
  the puddle-keep sum (`sumAll`) got the Water input, so painted pebbles also kill automatic puddles.
* The Water read sits behind a gate (`If` on the painted weight > 0), so unpainted ground does not pay the
  9 bombed taps. Water has no automatic rule.
* **Water brush** (`HE::water::brush`, `WaterBrush.cpp`): new `Params::pebbleLayer` (-1 = off) and
  `pebbleGroup`. Each paint dab calls `TerrainPaint::paintInGroup(layer, group)` over the brush's whole reach
  (radius + falloff, full strength) plus a bank half as wide again fading out (`kPebbleBankShare`); an erase dab
  calls the new `TerrainPaint::eraseLayer`, which hands only the Water weight back to layer 0 (painting layer 0
  would also thin out Grass/Dirt painted there). The eraser clears pebbles even when no water is left
  (the early return for an unallocated water grid comes after the pebble erase).
* **Editor** (`TerrainTools.cpp`, Water tab, section "Shore"): checkbox **Paint Pebbles** (`s_waterPebbles`,
  default **off**: opt-in as the step says), greyed with a hint when the material has no layer named "Water".
  The weight layer is looked up by *name* in the material's `graphLayerNames` (`waterLayerOf`), not hard-coded
  to 9, so a custom material with such a layer works and one without paints nothing. The group slot is the Paint
  tab's Active Group. Help entry `Landscape/Paint Pebbles##water`; `editor_help_audit.py --check` green.

## Weights travel

* Scene file: layer 9 lives in `layerWeights3` (`layerWeights3B64`); the new `test_water_brush.cpp` case
  "the weights save and load with the scene" round-trips all three pages after a pebble stroke.
* TerrainSync already ships `Weights3` (`TerrainSync.cpp:143`, `test_terrain_sync.cpp:202` paints layer 9 and
  compares host/client); the weights are sent from the grid bytes, not from the water body, so `brushBed`
  (not in the sync hash, `docs/water-brush-bed-2026-10-20.md`) does not matter for them.
* Undo: a stroke is one editor snapshot step (`snapshotNow`), which holds the weight pages with the water.

## Verified (macOS, M-series, Release)

* `he_tests` for `test_water_brush`, `test_terrain`, `test_landscape_groups`, `test_material_graph`,
  `test_auto_landscape_variants`, `test_terrain_tools_ui`, `test_terrain_mix_groups`, `test_terrain_sync`:
  238 cases, 297485 assertions, all green (includes the "shipped M_AutoLandscape*.hasset == builder" tripwire).
* New tests: pebbles off by default (no weightmap allocated); footprint + bank + sum 255; take from painted
  ground and erase only the pebbles (Dirt untouched, pebbles back to Auto); eraser works with no water left;
  mix-group share fed; scene round trip; `TerrainPaint::eraseLayer`; UI: Paint Pebbles checkbox + a drag +
  Ctrl-erase on the Forest material.
* Existing pins updated for the fourth bombed layer: 36 hex reads (72 `textureGrad`), 4 bombing grids, 6 layer
  blend nodes, WetGround slice now *read* by exactly the Water layer (6 sampler nodes).
* The four `M_AutoLandscape*.hasset` regenerated with `landscape_tex_gen EditorDeps/EngineContent/Materials
  --material [--group G]` (the generated `T_Landscape_*_Path_Albedo.hasset` it also drops into that folder were
  deleted: texture content is not committed).

## Not verified

* No live editor / no pixel check of the pebbles (the WetGround placeholder is a flat dark tone anyway).
  Metal/GL/D3D/Vulkan shader *compile* of the larger graph is covered by the existing cross-compile cases of
  `test_material_graph` (glslang GLSL/HLSL/SPIR-V), not by running a GPU.
* Linux/Windows: CI.

## Decisions taken / suggested

* Taken: Water reads WetGround (no 5th texture); opt-in default off; look up by name; footprint = brush reach
  plus 50 % bank; eraser = `eraseLayer`, not "paint Auto".
* Suggested for step 6: pack `ganges_river_pebbles` (Forest, Grassland) and `dry_river_pebbles` (Desert,
  Mountains) into slice 4 (WetGround; `autoLandscapeSliceSource` says slice `4*V`) of each group's
  Albedo/Normal/Mask arrays. `landscape_tex_gen` currently says "WetGround never packed" (`packGroupReal`,
  line ~837): that needs a `Water_*.png` input per group. Note WetGround is *shared* placeholder content so far.

## Handoff

* Files: `AutoLandscapeMaterial.h` (names, enum, comments), `AutoLandscapeMaterial.cpp` (`pebbles` read ~220, `wC`
  ~283, gate ~314, mixes ~420), `TerrainPaint.{h,cpp}` (`eraseLayer`), `WaterBrush.{h,cpp}` (`pebbleLayer`,
  `dabPebbles`), `TerrainTools.cpp` (`waterLayerOf`, `s_waterPebbles`, Shore section), `EditorHelp.cpp`.
* Next: step 6 packs the real pebble textures into the WetGround slice and checks the look; Thema 330 must keep
  channel 9 = Water.
* Trap: a local named `water` already exists further down `buildAutoLandscapeGraph` (the water ramp); the
  pebble LayerReads is called `pebbles`.
