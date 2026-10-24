# Puddle flicker, step 6: weather puddles of the auto-landscape material (Thema 234)

Result: on Metal, static camera, the auto-landscape weather puddles do **not** flicker. Their
material path has no time input, and measured frame-to-frame deltas are 0 (SMAA/no AA) or
indistinguishable from plain ground (TAA, GI). A moving camera shows higher puddle peaks (last bullet),
unproven as shimmer. Not run on Windows (this device is a Mac).

## Path (verified by reading code)
- The slider path is the auto-landscape graph, not the painted WaterSurface:
  `AutoLandscapeMaterial.cpp:295-310` calls `MF_WeatherPuddles` (`WeatherMaterialFunctions.cpp:59-79`)
  and derives the hollows from `Fbm(xz + offset, 1/Size)` (l.307-309). Column 8 (l.359+) is the overlay.
- No Time/ripple/rain/wind/wetness node anywhere in `AutoLandscapeMaterial.cpp` (grep: only `T::Weather`
  inside the function, pins Puddles + Size). So the water mask is a pure function of world xz, slope,
  height texture and two weather floats.
- The two floats: sole writer is `WeatherSystem.cpp:171,173` = `clamp(wx.puddleAmount/puddleSize)`, authored
  slider values, no lerp (the `drive` ramp at l.157-163 covers cloud/fog/wind/rain/snow/wetness only).
  Reaches the shader via `EnvironmentPush.cpp:42` -> `LightPacking.cpp:76-78`. Constant unless a script/panel moves it.
- GI side uses `puddleAmount` once per extract (`RenderExtractor.cpp:299-333`, `GiAutoLandscape.cpp:62`).

## Measurements (Metal, Apple M5, 1280x720, editor headless dump, GI/SSR/AA/bloom/DoF/MB off unless noted)
Delta = mean |d| per channel (0..255) between two separate processes, differing in one knob.
Binary: `out/deploy/Editor` of the main checkout (dated 22.10. 20:16, checkout at release/0.7.0 4dade2a2; this
branch only adds docs on top, so the puddle code is identical; deploy built from that commit NOT proven).
Mask: `AUTOLAND=masks` at the same camera, water = blue>128 and red,green<60; plain = all channels<30.
- Negative control, same settings twice: 0 px changed. Frames 16 vs 17/40: 0. `HE_SKY_TIME` 30/31/500: 0 px changed (921600 px).
- Positive control, WaterSurface (`WATERLAKE=l`) `HE_SKY_TIME` 30 vs 31: 9.25 % px changed, max 19. The method sees time-driven change.
- Mask view shows puddles present (25134 water px top-down, 47342 oblique).
- SMAA (AA=2) frame 16 vs 17/18/19: 0 water, 0 plain.
- TAA (AA=3) 16 vs 17/18/19, water | plain: changed 1.4/2.4/10.0 % | 1.9/2.5/5.8 %, mean 0.011/0.020/0.068 | 0.015/0.020/0.043, max 4-7.
  At f19 puddles are ~1.6x plain, absolute delta <0.1 of 255.
- GI on and GI + reflections (quality 2), f16 vs 17/18/19/24, water mean 0.06/0.12/0.17/0.37, plain 0.11/0.16/0.23/0.45:
  monotone growth with frame offset = accumulation converging, same on puddles and ground.
- Sun motion (oblique `CAMX=-52 CAMY=314 PITCH=-24 YAW=90`, TOD pinned per run), water | plain mean:
  TOD 0.300 vs 0.3005: 0.16 | 0.06 (2.7x), vs 0.302: 0.52 | 0.19; TOD 0.700 vs 0.7005: 0.26 | 0.41; vs 0.702: 1.03 | 1.67.
  Defaults are `dayNightCycle=false autoAdvance=false`; with it on, 120 s/day is ~1.4e-4 of a day per frame, smaller than any step tested.
  Smooth lighting change, not flicker; low sun reflections on puddles move ~2.7x more than ground.
- Camera shift (oblique, TOD 0.3, only X moved; control = same render twice, 0), water | plain, AA=0/SMAA/TAA nearly equal:
  1 cm: mean 0.14-0.16 | 0.16, max 22-31 | 8-15; 3 cm: mean 0.40-0.43 | 0.41-0.43, max 50-57 | 13-14;
  0.5 m: mean 3.63-3.65 | 1.49, max 107-118 | 76. Same mean, but puddles carry 2-4x higher peaks (sharp edge / mirror
  highlight) that AA does not reduce. No ground truth, so this is NOT proven shimmer; it is the best lead for the
  human's "permanent noise" (step-6 answer #147: weather puddles, constant noise, not a pop) with a MOVING camera.

## Reproduce (not in the tree)
`env HE_CONFIG_DIR=<tmp> HE_COLLAB_OFFLINE=1 HE_DUMP_PATH=x.bmp HE_DUMP_QUIT=1 HE_DUMP_RHI=Metal HE_DUMP_SKYTEST=1 HE_DUMP_TOD=0.4
HE_DUMP_COVERAGE=0 HE_DUMP_CLOUDMODE=0 HE_DUMP_CLOUDSHADOWS=0 HE_DUMP_CAMX=0 HE_DUMP_CAMY=400 HE_DUMP_CAMZ=0 HE_DUMP_PITCH=-89
HE_DUMP_GI=0 HE_DUMP_SSR=0 HE_DUMP_AA=0 HE_DUMP_BLOOM=0 HE_DUMP_DOF=0 HE_DUMP_MOTIONBLUR=0 HE_DUMP_RENDERPATH=0 HE_SKY_TIME=30
HE_DUMP_FRAMES=16 HE_DUMP_AUTOLAND=1 ./HorizonEditor` (cwd = deploy/Editor; same recipe as `scripts/auto-landscape-repro/cap158auto.sh`).
Vary one of FRAMES / SKY_TIME / AA / GI / GIREFL / TOD per run; diff with a pure-Python BMP reader.

## Not verified
- Windows/D3D11/D3D12/Vulkan/GL not run. No game or editor-Play run: the dump never ticks the world, so
  `puddleAmount` stability is from code reading, not a per-frame log from a running game. Terrain LOD/tessellation
  (feeds `slope`) stayed LOD0 (`HE_DUMP_TERRAINTICK` not tried). `HorizonGame` not run.
- Step 1 saw no flicker on the overlay either. Animated clouds in an oblique view were not tested (sun step only).

## Handoff
Decided: no fix (none warranted by data). Hypothesis only (not tested): with a moving camera the puddle's sharp
water edge (`ramp(0, 0.04)` in `AutoLandscapeMaterial.cpp`, waterMask) and mirror highlight alias; counter-test = widen the ramp
or blur roughness in a throwaway copy and re-run the camera-shift matrix, expect puddle max to fall to plain's.
Next steps, in order: (1) on Windows run the same matrix (frame count, AA, camera shift) via the game export recipe of
`puddle-flicker-repro-2026-10-21.md` with an auto-landscape + Weather entity scene, log `puddleAmount` per frame;
(2) the camera-shift test with a steady walk (several consecutive frames, not one pair) to see whether it is noise or motion;
(3) animated clouds + `autoAdvance` on, water vs plain mask.
