# Landscape 3.0 Folge, step 5: Mountains rock by size (2026-10-30)

Mountains keeps Dirt and Grass; only its Rock role changes. Other groups' materials are
byte-identical (their `M_AutoLandscape*.hasset` did not change).

## What it does

* `AutoLandscapeGroup::rockBySize` (Mountains only) makes the builder read Rock as three
  variants of the role (Thema 180 slice `g*V+v`), each pinned and tiled on its own:
  variant 0 medium (the old rock, "Rock Tile Size"), 1 large ("Boulder Tile Size" 12 m),
  2 small ("Pebble Tile Size" 1.5 m). A larger tile = the same photo covers more ground per
  repeat = larger blocks.
* Selection is by slope only (no height): small -> medium halfway between "Rock Slope" and
  "Boulder Slope" (0.40, about 53 degrees), medium -> large at "Boulder Slope", both ramps over
  "Rock Blend". Each size read sits behind an `If` on its ramp range +-0.01 and `rockCan`
  (so a size nobody sees costs nothing; painted rock gets the size by slope too).
* MaterialGraph: TextureArraySample / NormalMapArraySample get an optional appended `Variant`
  pin (3), the two ArrayBomb nodes one at pin 4. Wired: variant `clamp(v,0,V-1)` of the group
  instead of the per-hex hash. Unwired: GLSL unchanged (hash pin in test_auto_landscape_variants).
* 3 new params, Mountains 16/16 (`autoLandscapeParamCount(group)`); 312 nodes.

## Variant order for step 6 (pack real textures)

Mountains Rock: `Rock_<Map>.png` = medium (rocks_ground_08), `_v2` = large (rocks_ground_06),
`_v3` = small (rocks_ground_01), via `--pack`. On the current 1-variant arrays all three sizes
read the same slice: one rock at three tilings blended by slope, a visible change until packed.

## Verified (Mac, Release, 2026-10-30)

* he_tests built; `test_material_graph`, `test_auto_landscape_variants`, `test_landscape_groups`,
  `test_engine_materials` pass. The earlier guessed counts were wrong for the graph test: Mountains
  has 36 array reads (24 + 2 extra sizes x 3 maps x plain/bombed) and 6 `heBombGridV` calls.
* The variants test runs the Metal/GLSL/HLSL/SPIR-V cross-compile of the new graph.

## Not verified

* No Metal/GL screenshot (needs the editor build and a V=3 array: `landscape_tex_gen <dir>
  --variants 3` tints, see `auto-landscape-material-2-variants-2026-10-21.md` Traps).
* No perf capture: Rock goes from 9 to up to 27 taps on blend bands only.
* "Boulder Slope" 0.40 and the tile sizes are untuned.
