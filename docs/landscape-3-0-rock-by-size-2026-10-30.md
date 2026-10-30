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

## Witness (Metal and OpenGL, 2026-10-30)

Headless dump `HE_DUMP_AUTOLAND=1` (shipped material, 128 m analytic ramp, `HE_DUMP_AUTOLANDHEIGHT=30
HE_DUMP_AUTOLANDRAMP=56` for the "mid" slope, default 40/32 for "steep"), `SKYTEST TOD=0.4 AA=0
RENDERPATH=0`, camera `CAMX=-62 CAMY=326 PITCH=-24 YAW=90`. The arrays are the REAL Poly Haven rocks packed
to 1024² in a private HOME's EngineContentCache (Rock `_v2` = rocks_ground_06, `_v3` = rocks_ground_01, plain =
rocks_ground_08; Grass/Dirt/Snow stay placeholders), not committed: step 6 does the real packing.
Images: `docs/img-landscape-rock-by-size/`.

* V = 1 (`mid-v1-metal.jpg`): one rock photo over the whole mid slope. V = 3 (`mid-v3-metal.jpg`):
  small light stones on the gentle part, the medium rock below the boulder band, and on the steep
  face (`steep-v3-metal.jpg`) large boulder blocks. 25 % of the pixels differ from V = 1 by more than 12 levels.
* Metal and OpenGL, mid slope: mean |d| 0.05 (V = 1 and V = 3 alike). Steep face: mean |d| 8.2 (V = 1) and
  9.1 (V = 3), so the difference is not from this change; it sits at the foot of the ramp (Metal has the
  ramp's black shadow band, GL does not), the same Metal/GL shadow difference the older witnesses saw.
* Trap: in zsh `$CAM` does not word-split; use `${=CAM}` or the editor gets one garbage arg and renders sky only.
  A camera at CAMY >= 355 over this 340 m plateau also sees only sky.

## Perf (rough, Metal, M-series MacBook Air)

Real editor loop (`landscape_regression.py --detailed`, 200 warm-up + 500 frames, 2840x1528, vsync off),
128 m landscape of 1.8x the stock relief (`gen_autoland_scene.py` with the amplitude scaled, so slopes reach
the boulder band), 2 repetitions in alternating order. Screen LOCKED during the runs (the conditions files
say so), so absolute GPU times are inflated and noisy (C r1/r2: 6.0 / 8.2 ms); read the ratios.

| Cell | Scene pass p50 (r1, r2) | frame GPU p50 |
|---|---|---|
| A: material before this step (7e71f2d7), V = 1 | 3.7, 4.2 ms | 5.9, 6.9 ms |
| B: new material, V = 1 | 7.1, 6.9 ms | 10.0, 9.6 ms |
| C: new material, V = 3 | 6.0, 8.2 ms | 8.5, 11.7 ms |

* The rock-by-size material costs about +3 ms in the Scene pass on this rock-heavy frame (roughly 1.7x the
  Scene pass). V = 3 against V = 1 changes nothing beyond noise: the tap count is the same, only the slice differs.
* The 9 -> 27 Rock taps happen only on the blend bands, but in a steep scene the bands cover a lot of the
  screen, and the `If` gates are per pixel (neighbouring pixels in different bands diverge). Not measured:
  a flat/gentle view (should be close to A), nor the cost split between taps and the bombing hex math.
* If this is too much for the shipped Mountains: cut the medium band (blend small <-> large directly, 2 reads),
  or make the small size an `If` on a larger slope margin. Left as is; the human did not ask for a budget.
* Not measured: D3D/Vulkan (no hardware here), OpenGL perf.

## Not verified

* Boulder Slope 0.40 and the tile sizes (12 m / 1.5 m) are untuned against real, full-size (2K) Mountains arrays.
* No CI run for these commits yet (Linux/Windows `test_material_graph` counts 36 / 6).

## Handoff

* **Done / verified:** graph + params + regenerated `M_AutoLandscape.hasset` (`AutoLandscapeMaterial.cpp`, 2f7a0a4d);
  4 target tests green on the Mac; Metal+GL witness with real rocks (above); rough Metal perf (above).
* **Decisions:** variant order 0 medium / 1 large / 2 small; slope only; Mountains only (other groups byte-identical).
* **Not verified:** D3D/Vulkan runtime, 2K real arrays, tuning of Boulder Slope / tile sizes, CI.
* **Next (step 6):** pack `Rock_<Map>.png` (rocks_ground_08), `_v2` (06), `_v3` (01) for Mountains with `landscape_tex_gen --pack`
  (zips are in ~/Downloads, `scripts/landscape-textures/stage_polyhaven.py` stages them; needs numpy/Pillow/OpenEXR in a venv,
  run with `python3 -I`), then re-shoot with the recipe above and tune. If the +3 ms Scene cost matters, see Perf.
