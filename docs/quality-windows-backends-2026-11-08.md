# Quality settings on Windows: D3D11, D3D12, OpenGL (and Vulkan on the real GPU)

Hive topics 324 (central quality settings) and 336 (HorizonCode access, shader and automatic quality), the
Windows steps: 336 step 7 (D3D11/D3D12), 8 (Vulkan), 9 (OpenGL), 10 (automatic quality), plus the "D3D acceptance" note on
topic 324. Done 2026-11-07/08 on NN-WS03 (Ryzen 9 9950X, RTX 4070, driver 32.0.16.2101), branch
`claude/quality-windows-backends`, cut from `claude/hc-engine-settings-nodes` (eb5f65bc6). Release build, hidden headless
editor dumps, no debug layers unless said (section 8). Harness: `scripts/he_quality_windows_matrix.ps1`. Images:
`docs/quality-windows-backends-2026-11-08/`.

## 1. Result

| What | D3D11 | D3D12 | OpenGL | Vulkan |
|---|---|---|---|---|
| Presets Low/Medium/High/Ultra change the image (section 3) | acts | acts | acts | acts |
| Material quality lane (Quality Level node, noise octaves) (section 4) | acts | acts | acts | acts |
| Shadow resolution / cascades / distance (section 5) | acts | acts | acts | acts |
| Texture quality, before this work | **ignored** (0.00) | **ignored** (0.00) | **ignored** (0.04) | acts (10.66) |
| Texture quality, now (section 2) | acts (10.67) | acts (10.67) | acts (10.57) | acts (10.66) |
| Quality change DURING a run reaches the live renderer (section 6) | same frame as a direct run | same | same | same |

Numbers in the texture row are the mean absolute pixel difference (0-255) of High (2) against Low (0), camera 0.5 m above the
ground; the noise floor of a repeated run is 0.00-0.09.

## 2. What was missing and was built

Only texture quality. Everything else already acted on the three backends the topic doc listed as "unchecked" (the old table said
shadows were "ignored" on D3D11/D3D12; they were not).

* **D3D11** (`D3D11Renderer.cpp`): the scene sampler `sampler`, the graph-material sampler `m_matSampler` and the decal sampler
  start at mip level 2 / 1 / 0 (Low / Medium / High). D3D11 sampler states are immutable, so `applyTextureQuality` re-creates the
  three from their own description when the quality changes; the draws bind them from the ComPtrs every frame. `samplerFull` is
  the unclamped copy the asset previews keep (like Metal's decision: previews, UI and weightmaps keep full detail).
* **D3D12** (`D3D12Renderer.cpp`): the samplers are static in the root signatures, so the clamp lives on the view:
  `srvForTexture(..., minLod)` sets `ResourceMinLODClamp` on the Texture2D / Texture2DArray views of the world's colour textures
  (heTex0, heTexP0..3, mesh / skinned / override / decal base colours). The views that already sit in the scene heap are rewritten in
  `applyTextureQuality` after `waitForAllFrames()`. UI, GI and weightmap views stay at 0.
* **OpenGL** (`OpenGLRenderer.cpp`): GL has no sampler objects here, so `GL_TEXTURE_MIN_LOD` is set per texture: at upload for
  every world texture (UI images are excluded) and on the live ones (mesh, skinned, override, graph / landscape arrays) at the top
  of the next `DrawScene`, where the context is current.
* All three override `SetQualitySettings` like Metal and Vulkan do. Log lines: "texture quality ... start at mip level N".
* **Shadow rebuild log line** on D3D11, D3D12 and OpenGL, the same text Vulkan already had ("shadow cascades rebuilt at 4096x4096 (3
  cascades, distance 500)"), so a capture can prove the setting arrived.
* **Dump knob** `HE_DUMP_QUALITYSWITCH="tex,mat[,shadowResolution]"` (`EditorApplication.cpp`): the capture's knobs are the start state,
  the settle frames render there, then the new values are pushed and the frame is taken. This is the path the governor takes.

Texture quality only changes textures that have a mip chain; a single-level texture looks the same (as on Metal). The landscape
weightmaps in `m_graphTexCache` on OpenGL go through the same clamp; whether the shipped weightmaps have a mip chain was not checked; the
Auto Landscape layer masks looked unchanged at Low in the preset images.

## 3. Presets (images: `presets-<backend>.png`, Low | Medium | High | Ultra, a quarter of 1280x720)

Scene: `HE_DUMP_AUTOLAND=1` (the shipped Auto Landscape material on a 128 m relief), `HE_DUMP_SKYTEST=1`, `HE_DUMP_TERRAINTICK=1`,
`HE_DUMP_TOD=0.5`, camera (-10, 335, 75) pitch -22. Mean absolute pixel difference (0-255) / percent of pixels differing by more than 8:

| Pair | D3D11 | D3D12 | OpenGL | Vulkan |
|---|---|---|---|---|
| noise floor: High vs High again | 0.05 / 0.0 | 0.09 / 0.0 | 0.02 / 0.0 | 0.07 / 0.0 |
| Low vs Medium | 2.21 / 6.4 | 2.26 / 6.4 | 2.17 / 6.5 | 2.24 / 6.4 |
| Medium vs High | 2.82 / 11.4 | 2.87 / 11.4 | 2.98 / 12.4 | 2.85 / 11.4 |
| High vs Ultra | 1.56 / 2.5 | 1.68 / 3.1 | 1.67 / 3.1 | 1.72 / 3.0 |
| Low vs Ultra | 4.65 / 11.7 | 4.75 / 11.9 | 4.72 / 12.9 | 4.80 / 12.0 |

Every preset step is visible and the four backends agree on its size within a few percent.

## 4. Material quality (images: `material-quality-<backend>.png`, 2 | 0)

Same scene, High preset, `HE_DUMP_MATQUALITY` 2 against 0: mean 2.81 (D3D11), 2.86 (D3D12), 2.79 (OpenGL), 2.86 (Vulkan), 7.5 % of
pixels over 8 on all four. The lane (`heLight.counts.y`, written by `FillMaterialWind`, which D3D11 and D3D12 call in their material
fill) reaches the HLSL and GLSL noise helpers; the adaptive octave loops compile with FXC and the GL driver (no X3570).

## 5. Shadows (images: `shadow-256-vs-4096-<backend>.png`: 256 px map, 4096 px map, difference x4)

`HE_DUMP_SHADOWINSTTEST=1` (row of cubes over a floor), TOD 0.35, **GI off** (with GI on the sun shadow comes from the ray-traced
mask and the cascade size cannot be seen), `HE_DUMP_SHADOW=100,2,256` against `500,3,4096`. Mean difference 0.89 (D3D11), 0.89 (D3D12),
0.61 (OpenGL), 0.90 (Vulkan), 2.1-2.2 % of pixels over 8; repeated 4096 run 0.00-0.02. 256 px shows the terraced edges, 4096 px the
crisp ones, on every backend. Each run logs the rebuild line.

## 6. A change during a run (the governor's path)

`HE_DUMP_QUALITYSWITCH` renders the settle frames at the start state, pushes the new values, renders four more frames and captures.
Compared with a plain run started at the NEW values and the same frame count (mean abs difference):

| Switch | D3D11 | D3D12 | OpenGL | Vulkan |
|---|---|---|---|---|
| texture quality 2 -> 0 | 0.00 | 0.00 | 0.02 | 0.01 |
| material quality 2 -> 0 | 0.05 | 0.08 | 0.02 | 0.02 |
| shadow resolution 4096 -> 256 | 0.02 | 0.02 | 0.00 | 0.01 |

(The first version of this check compared against a run with 3 instead of 7 frames and showed 3.4-3.9 on the texture row: that was GI's
temporal accumulation, the same on every backend, not a switch error.) So the live sampler re-creation (D3D11), the SRV rewrite
after a GPU flush (D3D12) and `GL_TEXTURE_MIN_LOD` on live textures end in exactly the state a fresh start has.

## 7. Times (whole-frame GPU ms, median of 200 frames, two runs each, 1280x720)

Same scene as section 3, runs interleaved, `HE_DUMP_GPUTIME=200`. The two run values are in brackets.

| Case | D3D11 | D3D12 | OpenGL | Vulkan |
|---|---|---|---|---|
| preset Low | 11.40 (13.38 / 9.41) | 5.47 (6.21 / 4.74) | 2.65 (2.66 / 2.65) | 5.15 (5.29 / 5.01) |
| preset Medium | 10.19 (8.35 / 12.04) | 5.46 (5.52 / 5.41) | 2.92 (2.92 / 2.93) | 5.48 (5.82 / 5.15) |
| preset High | 30.78 (30.83 / 30.72) | 5.40 (5.80 / 5.01) | 3.11 (3.11 / 3.10) | 4.73 (4.70 / 4.76) |
| preset Ultra | 30.61 (30.58 / 30.65) | 6.92 (7.19 / 6.66) | 7.90 (7.88 / 7.93) | 6.37 (6.38 / 6.36) |
| High, material quality 0 / 1 / 2 | 30.59 / 30.59 / 30.75 | 5.41 / 4.97 / 5.08 | 3.08 / 3.08 / 3.12 | 4.83 / 4.86 / 4.99 |
| High, texture quality 0 / 2 | 30.67 / 30.54 | 4.99 / 5.00 | 3.10 / 3.11 | 4.78 / 4.86 |
| High, GI off | 7.26 (5.31 / 9.21) | 4.94 | 2.93 | 4.79 |

Reading it:
* **Material and texture quality cost nothing measurable** on any backend (within the run-to-run spread, 0.1-0.5 ms); their gain is
  in the image and in memory bandwidth on larger scenes, not in this witness.
* **GI is the preset cost on D3D11:** software compute ray tracing, 11-24 ms of the High frame here; D3D12 does it with DXR in under 1 ms
  (High 5.4 against GI off 4.9). OpenGL's Ultra adds 4.8 ms over High (what in Ultra costs it was not split out).
* **The D3D11 numbers are not clean.** Single runs land on two plateaus, 16.7 ms and 30.7 ms (seen at High: 16.66 / 16.75 / 30.75 in
  three runs of the same setup, minutes apart), and Low/Medium vary 8-13 ms. That is a measurement problem (the whole-frame timer, short runs
  at idle GPU clocks, other work on the machine - a bee held the shared build tree next door and may have run too), not a quality effect; do not read the D3D11
  High figure as a stable frame time. A locked-clock run on an otherwise idle machine and a look at the D3D11 timer are still to do.
* Short runs (5 s each), GPU clocks not locked.

## 8. Validation and tests

* Debug layers on (`HE_GPU_DEBUG=1`), texture-switch and shadow-switch runs on all four backends: 0 errors. D3D12: only the known
  clear-value warnings (the same three seen before this work) and, in the landscape scene, "Pixel Shader expects a Render Target View bound
  to slot 3" at pipeline creation (the material variant's extra output, not this work). Vulkan validation: one notice, "vertex
  attribute at location 2 not consumed". D3D11 and OpenGL: nothing.
* `he_tests` (built in this tree): test_quality_governor 17/17, test_quality_enums 8/8, test_engine_materials 15/15, test_material_graph
  126/126 (these include the D3D12 root-signature PSO checks on WARP), test_project_settings_ui 9/9, test_engine_api 130/130. The full
  suite was not run in this pass.

## 9. Not done

* **The governor in a real exported game on Windows** (step 10's real-display part). This machine has no export path ready: the
  editor's export builds the game from the SDK and needs an export profile and a project from the editor (the live export scripts
  expect `~/Documents/HorizonEngine/HorizonTutorial`, absent here), and a game folder assembled by hand starts without a scene (it loads
  the startup scene from the pak config). What IS covered: the governor's logic (17 unit tests), its renderer-side effects through
  section 6 (the same `SetQualitySettings` / `SetShadowSettings` calls it makes), and the tick wiring is the Metal-verified one in
  `GameApplication.cpp`. A windowed, vsynced run is also not done.
* **Hardware detection / benchmark** (the human's note on topic 324, 28 Oct): not built; the hook is `setPreset` plus
  `detectQualityPreset`, and the timings above are the kind of input a benchmark would use.
* **D3D11 GPU timer plateaus** (section 7) and a locked-clock run.
* OpenGL texture quality clamps by texture parameter, so a texture shared between a world use and a UI use would be clamped for both;
  UI images are separate uploads (`m_uiTexCache`), so this does not happen today.

## 10. Files

`D3D11Renderer.{h,cpp}`, `D3D12Renderer.{h,cpp}`, `OpenGLRenderer.{h,cpp}` (texture quality, shadow log line), `EditorApplication.cpp`
(`HE_DUMP_QUALITYSWITCH`), `scripts/he_quality_windows_matrix.ps1`, this doc and its images.
