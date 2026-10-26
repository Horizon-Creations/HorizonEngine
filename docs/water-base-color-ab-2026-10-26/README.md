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
