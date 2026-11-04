# Landscape: Rock by slope in every group, desert sand colour, new Bare Soil (2026-11-04)

Branch `release/0.8.0`. Commits: `e061af08` / `d7c43990` (sand colour), `f84775b7` (rock by slope, all groups + mix) and
the one holding this doc.

## What changed

* **Rock by slope in all four groups and in the mix.** The Rock role (Mountains Rock, Forest Mossy Rock, Grassland Stones,
  Desert Sandstone) is no longer one texture with random hex variants but three textures chosen by the slope:
  variant 1 **medium** (`Rock Tile Size`), 2 **large** (`Boulder Tile Size`, 12 m: steep faces), 3 **small**
  (`Pebble Tile Size`, 1.5 m: stones on gentle slopes). Two slope ramps blend them (small -> medium halfway between
  `Rock Slope` and `Boulder Slope`, medium -> large at `Boulder Slope`, both over `Rock Blend`), every read behind an
  `If` gate on its own ramp, so a size nobody sees costs nothing. New per-group default `Boulder Slope`:
  Mountains 0.40, Forest 0.35, Grassland 0.40, Desert 0.55. Three new parameters per group (Mountains/Forest/Grassland
  18, Desert 19, Mix 19 of 32 after the limit raise).
  The graph code is the "rock by size" work of the unmerged Folge branch (`2f7a0a4d`), ported: the optional `Variant`
  pin on the four array sample nodes (`MaterialGraph.cpp`, unwired = GLSL as before) and the Rock block of
  `AutoLandscapeMaterial.cpp`, now for every group (`AutoLandscapeGroup::rockBySize`, `boulderSlope`).
* **Mix material**: the fat arrays now hold V variants of every group's role, slice `(group * 4 + role) * V + variant`
  (V = 3 -> 48 slices, 1.02 GB per array); `landscape_tex_gen --mix` copies all of them. The mix shader already divided
  the depth by 16, so the other roles also get their per-hex variants there now (before, the mix had variant 0 only).
* **Textures** (`scripts/landscape-textures/variant_sets_download.py` holds the plan):

| Group | Role | v1 medium | v2 large (steep) | v3 small (gentle) |
|---|---|---|---|---|
| Mountains | Rock | existing | rock_face_03 | rock_05 |
| Forest | Mossy Rock | mossy_rock | lichen_rock | rock_3 |
| Grassland | Stones | rocks_ground_03 | rock_boulder_dry | pebble_ground_01 |
| Desert | Sandstone | rock_06 | cliff_side | dry_riverbed_rock |

  Roles other than Rock keep their random hex variants. **Grassland Bare Soil** lost the two sets with footprints
  (brown_mud_03, brown_mud_02): now forest_ground_05 and dry_ground_rocks, variant 3 repeats variant 1. Both are
  closer in colour to another layer than the rule you set allows (forest_ground_05 to Forest Floor's
  brown_mud_rocks_01, dry_ground_rocks to Desert Hardpan's sandy_gravel_02): decided "use them anyway" after the
  other candidates were all brown soils close to something; swap them if they get confused in a mixed terrain.
  rock_face_03 no longer sits in two groups (Forest's third variant was a copy of Mountains' large rock).
  Still shared: `grass_ground` and `grass_path_3` between Mountains Grass and Grassland Meadow.
* **Desert sand colour**: sRGB ~142/110/80 (was 129/114/91), between the reference's grey-tan and the textures' red.
  `docs/img-landscape-slope-variants/desert-sand-colour-old-new.jpg`. (A first try at 150/108/76 read as orange.)

## Verified

* Tests (`C:/hb/relb`, Release): `test_material_graph` 123/123, `test_engine_materials` 15/15, `test_landscape_groups`
  8/8, `test_auto_landscape_variants` 10/10 (new case: three sizes and `Boulder Slope` defaults per group, mix reads
  them), `test_material_tessellation` 15/15. Pinned counts updated: 30 array reads and 5 `heBombGridV` call sets for
  the Mountains graph.
* D3D11 witness of the four groups at a steep face (`HE_DUMP_AUTOLANDHEIGHT=40/RAMP=32`, `HE_DUMP_TOD=0.62` lights the
  face): each group's steep face is a different, clearly distinct texture (sandy cliff, dark lichen cliff, pale boulder
  rock, striated sandstone), small stones show at the foot.
  `docs/img-landscape-slope-variants/rock-by-slope-groups-d3d11.jpg`.

## Not verified

* The mix material with V = 3 on a GPU (the HEProject terrain is a mix): the arrays build and the graph generates, the
  `HE_DUMP_AUTOLANDMIX` witness was not run on the new arrays. Cost: three rock reads per side, each doubled in a mix.
* Metal/ASTC, D3D12/Vulkan/GL frames of the new graph (D3D11 only; the Variant pin is plain GLSL integer code).
* Boulder Slope values and tile sizes are untuned guesses per group; the `Pebble` read at 1.5 m shows only on gentle
  rock slopes, which the witness ramps barely have.
* Perf (three rock reads instead of one on the slope band).

## Handoff

* **Done:** engine graph + tests + regenerated Mountains/Forest/Grassland/Desert/Mix materials, packed arrays (4 groups
  x 3, mix x 3), D3D11 witness; upload of the 15 arrays started (see below), deploy after it.
* **Files:** `src/HE_Core/{include,src}/MaterialGraph/AutoLandscapeMaterial.*` (`rockBySize`, rock block),
  `MaterialGraph.cpp` (Variant pin), `src/HE_Tools/src/LandscapeTexGen/main.cpp` (`writeMixArrays`),
  `scripts/landscape-textures/variant_sets_*.py`, tests.
* **Traps:** `he_tests` loads `HorizonCore.dll` from `tests/`: copy the new one there after an engine-only change
  (a stale one made two baked-material tests fail); `HE_ENGINE_SDK_DIR` is separate from `DEPLOY_DIR`; the Mix arrays are
  3 GB for three maps (48 slices); the editor cooks them to BC7 on load (`GpuArrayCook`).
* **Next steps:** run the mix witness / look at it in the real project; tune `Boulder Slope`; decide Bare Soil confusion.
