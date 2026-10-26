# Water base colour: the A/B photos and the shore (Thema 320)

Follow-up to Thema 230 / PR #185. The human wants the engine water to read as water and as
transparent, with a black base colour. Step 1 shot two ways of doing it; this folder keeps the
photos (they were only in a git-ignored scratch dir) and the numbers; the implementation of the
human's decision is in the second half of this file (added by step 2).

## The photos

27 JPEGs (quality 85 of the original PNGs), named `<variant>_<view>_tod<T>.jpg`.

* Shot by run 1565 (step 1) on **Metal**, base `b128c97b` (the water code is unchanged up to the
  release head this step starts from), `HE_DUMP_WATERBRUSH=dig`, `HE_DUMP_SKYTEST=1`, empty
  `HOME`, 1280x720. They are not re-renders: the step text called them lost, they were still in
  `_shots/water-black-base-320/` of the main checkout (git-ignored), so they were copied, not shot
  again.
* Variants. `before` = the shipped material. `A` = black base colour only (the body colour chain
  is thrown away, foam kept; `variant_A_black_base.patch`). `B` = black base plus the body colour
  put into *Emissive* (`variant_B_black_base_emissive.patch`).
* Times of day (`HE_DUMP_TOD`): `0.5` day, `0.3` dusk, `0.0` night.
* Cameras: `top` (`CAMY=350 CAMZ=44 PITCH=-58`), `mid` (`CAMY=316 CAMZ=52 PITCH=-18`), `graze`
  (`CAMY=306 CAMZ=60 PITCH=-6`).

## What they show (mean RGB of a rectangle inside the water, `/tmp` script on the PNGs)

| view | variant | day | dusk | night |
|---|---|---|---|---|
| top | before | 56 96 121 | 21 38 55 | 20 36 57 |
| top | A | 45 50 56 | 18 22 27 | 17 19 24 |
| top | B | 53 88 106 | 29 68 88 | 27 66 87 |
| mid | before | 44 92 131 | 37 62 91 | 13 26 48 |
| mid | A | 37 58 85 | 36 53 74 | 11 12 18 |
| mid | B | 43 85 120 | 41 80 112 | 18 49 74 |
| graze | before | 88 143 181 | 84 123 156 | 20 33 60 |
| graze | A | 84 126 162 | 83 118 149 | 18 22 34 |
| graze | B | 88 140 176 | 86 133 166 | 24 54 83 |

* **A** is dark everywhere the view looks down into the water and takes its colour only from the
  sky reflection: grey-black from above (day 45 50 56), deep navy at grazing angles, black at night.
  The ground shows through the alpha, so from above it is also the greyest.
* **B** looks like the shipped water again, because Emissive is unlit: it is as bright at night
  as at noon (night `top` 27 66 87 against 20 36 57 before) and its tint is the saturated teal/blue
  of `ShallowColor`. That is the "too bright, too blue" the human saw.
* Neither variant touches the shore: the alpha ramp, the foam fringe and the clear-water opacity are
  the same in all three.

The patches are against `src/HE_Tools/src/MatGen/main.cpp` at `b128c97b`; neither was applied as
written (see below).

# Step 2: realistic depth colour and a soft shore

The human's verdict on the photos above: B is too bright and too blue, A is too dark from above and at night,
a distinction by depth is welcome, and the real complaint is the line where the water meets the landscape.

## What the hard line was

A black-base variant would not have removed it; it only made it easier to see. Measured on the dig scene
(`WATERBRUSH=dig`, top camera, day) the luma fell from the ground's to the water's within about 16 px for most of the step
(10 to 90 % of it: 20 to 34 px, steepest 0.05 to 0.11 of the step per pixel). Three probes of the graph
(depth channel `e`, the alpha, `ramp`/`trans`/`bodyA` as colour) showed why: the material has no scene depth,
the only depth it knows is the one `WaterMesh` bakes into the vertex normal, and that stopped at
`kShoreFadeDepth` = **1 m**. Opacity (`opacEff`), absorption (`trans`) and the alpha ramp all had to finish
inside that metre, so the water reached its deep value at one contour line (about 0.5 m deep). Widening the
knobs inside that metre does not help (tried: it only made the edge harder, see "Tried" below); the data had to
reach further.

## What was done

* `Physics/WaterWaves.h`: `kShoreFadeDepth` 1 m -> **3 m**. The mesh carries `e = 1 - depth / 3 m`; the
  gain of the tilt, the readback in `Water.hasset` and the foreign-material tilt (at most 1.8 degrees, at the
  waterline) are unchanged. `WaterMesh.cpp` refines to the same 3 cm of water as before (`kTolerance` is
  now `0.03 / kShoreFadeDepth` of `e`), so the mesh near the waterline is what it was.
* `MatGen/main.cpp` (all of it in the water graph; the underside branch is untouched):
  * the body colour from above is `lit` times a gain that runs from 0.85 (the light reaches the floor,
    `trans` -> 1: the shallows) to 0.50 (deep water), after 12 % of its saturation is taken out
    (`kBodyShallow/Deep/Mute`). Emissive stays 0 (A, not B), so the night is dark, and the colour is the
    existing muted `ShallowColor`/`DeepColor` pair, not the saturated light blue;
  * the shipped shallow-water defaults reach across the new range: `ShallowWater.x` (clear depth)
    0.8 -> 1.5 m, `Turbidity.z` (shore width) 0.35 -> 0.6 m. `tintStart` (0.5), the opacity floor (0.25), the
    absorption (0.45/m) and the assumed depth (4 m) are as they were: open water (e = 0) is unchanged;
  * tooltips (`MatGen`, `EditorHelp.cpp`, `he-docs.json`) say "three metres".
* `Water.hasset` regenerated with `mat_gen` twice, `cmp` equal; UUID {0x600,1} and the file version unchanged.

## Photos (`shore/`, before on the left, after on the right)

`<view>_tod<T>_before-after.jpg`, Metal, `HE_SKY_TIME=1`, `RENDERPATH=0`, `AA=0`. `top`, `mid`, `graze` are the
three cameras of the A/B set on the dig scene (day 0.5, dusk 0.3, night 0.0); `beach` and `shallow` are the
gentle sloped beaches of `scripts/water_baseline_shots.py`, `shore` is the bowl lake. `top_west_end_zoom_*` is the
west end of the dig pool at 4x, day: the crisp edge of the left half is the line the human saw.
"Before" was shot with the editor and `Water.hasset` of `b128c97b`, "after" with this branch's editor and hasset.

| view | | day | dusk | night |
|---|---|---|---|---|
| top | before | 56 96 121 | 22 38 55 | 20 36 57 |
| top | after | 125 149 159 | 48 64 79 | 46 63 84 |
| mid | before | 44 92 132 | 37 62 91 | 13 26 48 |
| mid | after | 79 113 138 | 46 69 93 | 25 36 53 |
| graze | before | 90 144 182 | 85 124 157 | 20 33 60 |
| graze | after | 91 138 174 | 85 122 154 | 21 29 48 |

(Mean RGB of a rectangle in the water. The `top` rows are lighter than before because the dug pit is 1.5 m
deep over a white floor: the water there is real shallow water and lets the floor through; on deep water the
mean falls toward the `mid` numbers. At grazing angles it is reflection and is as it was.) Nothing here is
a saturated light blue: day `mid` is 79 113 138 against 44 92 132 before, and the deep water darkens at dusk and night.

Shore sharpness (`/tmp` script on the PNGs, rows through the west and east end of the pool, day, 10 to 90 % of
the luma step in pixels, steepest luma step per pixel as a share of the whole step):

| row | before W / E | after W / E |
|---|---|---|
| 285 | 22 px (0.079) / 31 px (0.063) | 39 px (0.033) / 43 px (0.036) |
| 310 | 30 px (0.050) / 34 px (0.057) | 44 px (0.023) / 45 px (0.031) |
| 260 | 20 px (0.114) / 31 px (0.063) | 21 px (0.108) / 40 px (0.034) |

Row 260 cuts the pool's curved end at a slant and shows no gain on the west side: that is the pool's outline,
not the depth fade. At night the same image: 29 to 39 px before, 28 to 54 px after.

## Underwater and open water

With the same editor, the old and the new `Water.hasset` give **byte-identical** bmps for the underside views of
`scripts/underwater_look_shots.py` (`up`, `zenith`, `deep_up`, `horizon`, `floor`, `shore`, each with and without
fog). `dry_shallow` (a view from above) differs, as it must: the step's "dry_shallow md5 stays" cannot hold once
the colour from above changes. The old editor (`b128c97b`) and the new one (`f098fdbc`, with the frame-perf
levers of #186) already differ in `up`/`horizon`/`dry_shallow` with the same hasset, so compare with the new editor.

## Tried

* Only the ShallowWater/Turbidity defaults at the old range (tint start 1.0, clear depth 1.0, shore width 0.6/0.9):
  the edge got harder, because the tint path then runs from the real depth to the assumed 4 m inside the same metre.
* An alpha ramp built on the open-water transmittance and an eased ramp `1 - (1-s)^1.5`: the same contour at the
  same depth, no gain. Reverted.
* Variant A as written (`base = black`): throws the whole body-colour chain away, so a depth colour is impossible.

## Not verified

D3D11, D3D12, Vulkan and OpenGL were not rendered (this is the Mac); the change is the mesh constant and the shared
graph, no backend code. A real 1 km landscape (terrain LOD) and a rotated or non-uniformly scaled landscape were
not shot (the readback test of step 8 still passes). The human's own Windows deploy has its own `Water.hasset`
and sees the change only when that is refreshed.

## Tests

`tests/test_water_surface.cpp` (57 cases): the new "the depth runs on to kShoreFadeDepth with no plateau on the
way" (a 10 cm/m bank, lake deeper than the range: every vertex and every triangle centre follows the real depth to
3 m, vertices past the old metre are not 0, past the range exactly 0); the plane-bank case now reads the range from the
header; `solidIndices` ignores slivers under 1e-4 m² (the island case had two of 1e-5 to 3e-5 m² on a straight run of
contour points, 10 to 20 µm off their chord, which the T-junction check read as a crack; they draw nothing).
`tests/test_engine_materials.cpp` (13 cases): the knob pins (`Turbidity.z` 0.6, `ShallowWater.x` 1.5) and "the shore
depth range it reads is the one the water mesh bakes" (the baked graph holds `kShoreFadeDepth` and
`kShoreNormalGain`, the clear depth and the ramp end inside the range). The full `he_tests` has one environmental
failure on this Mac, `Disk registry: scanContentDirectory resolves UUIDs from loose content` in `test_hpak.cpp`
(6 == 2): `scanContentDirectory` also scans the user's `~/Library/Application Support/HorizonEngine/EngineContentCache`;
with an empty `HOME` it passes.
