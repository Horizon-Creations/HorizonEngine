# Procedural snow and sand, mixing, parameters and trails: state, cost, trail data model (Thema 328, step 1)

Docs and measurement only; no engine code changed. Written 29.10.2026 on the Mac (Metal), after a first attempt on NN-WS03 (Windows)
that found the CI result below and was moved before it could measure.

## 0. Short version

| Question | Answer |
|---|---|
| Are `MF_ProceduralSnow` / `MF_ProceduralSand` available? | **Only on the 329 branch** (`claude/landscape-3-0-folge-path-layer-prozeduraler-schnee-sand-mate`, step 2 = `59c92753`..`cfa5bcb1`). Not in `origin/release/0.8.0` (`git merge-base --is-ancestor cfa5bcb1 origin/release/0.8.0` is false) and not in this branch. Topic 326's branch `claude/landscape-weltmassstab-100km` does not exist on origin yet, and its plan doc (`docs/landscape-world-scale-plan-2026-10-28.md`) is not committed anywhere. |
| Was 329 step 2 CI-green? | **No, not at `cfa5bcb1`.** Run 40879630317: macOS and Linux·Vulkan green, **Linux and Windows red**, same two assertions (`tests/test_material_graph.cpp:3746`, `MF_ProceduralSnow` and `MF_ProceduralSand` not equal to what the builders make). Cause (found independently here from the CI logs, and the same one 329 step 7 found): unspecified C++ argument evaluation order in `ProceduralSurfaceFunctions.cpp` (`add(mul(a,b), mul(c,d))`), so Clang on the Mac numbered nodes differently from GCC/MSVC. Shader text and the baked materials were never affected (`test_landscape_groups` stayed green). **Fixed by `d27fad34` on the 329 branch; a new CI run (40942374987) was still running when this was written.** |
| What do the functions cost? | Measured on Metal, per group (section 2): the step-2 materials are **cheaper** than the old texture-snow materials, by 2.7 to 2.9 ms of the Scene pass (-17 to -19 %) looking down on terrain and 0 to 1.4 ms (0 to -10 %) at gameplay height. D3D/Vulkan/GL not measured. |
| Trail data model | World-space two-level clipmap on the GPU (one new texture binding, read with integer fetch so it needs no sampler), a sparse 128 m page store on the CPU as source of truth, 8-bit depth, 0.25 m texels. Section 3. |
| Does Disturbance touch `Height`? | **No.** Trails change Albedo, Normal, Roughness and AO only; `Height` (and with it `r.snowMask`) stays weather-free and state-free. Section 3.5. |
| Open questions for the human | Section 6. |

## 1. Prerequisites, checked

| Check | Result | How |
|---|---|---|
| Functions exist | yes: `ProceduralSurfaceFunctions.h/.cpp`, assets `EditorDeps/EngineContent/MaterialFunctions/Surface/MF_ProceduralSnow\|Sand.hasset`, ids `0x41D`/`0x41E` | read on `origin/claude/landscape-3-0-folge-...@cfa5bcb1` |
| In `release/0.8.0` | no | `git merge-base --is-ancestor`, after `git fetch origin` |
| In this branch | no (it starts at `852f37b7`, before step 2) | same |
| CI at `cfa5bcb1` | red on Linux and Windows, see section 0 | `gh run view 40879630317`; the two failing JSONs were pulled out of the job log and diffed: 18 636 / 18 989 characters each, first difference at the source pin of the `lenSq` multiplies (`"sp":0` in the committed asset, `"sp":1` fresh) |
| CI after the fix | pending | run 40942374987 (`gh run list --branch claude/landscape-3-0-folge-path-layer-prozeduraler-schnee-sand-mate`) |
| Topic 326 tile format | **does not exist in code.** Only its plan numbers (post of 28.10.): heights 16 bit at 2 m, weights 2 pages RGBA8 at 4 m, quadtree patches of 64 cells, resident radius ~8 km | `hive_read_topic 326`; the trail design below is made against these numbers and says where it depends on them |

What this means for the next steps: 329's branch has to merge into `release/0.8.0` (or 328 has to be rebased on it) before steps 3 to 5 can be built; step 1 of 326 has to exist before step 6 (persistence) can be implemented, but not before it can be designed, and section 3.6 is written so that a late 326 changes only the page key.

## 2. Cost of the functions

**Result: on Metal the procedural snow and sand are a net saving, not a cost.** Replacing the texture snow role (all groups) and the Desert sand role by the two functions removes more texture work than the noise adds.

### 2.1 How it was measured

* **A/B of one binary.** `old` and `new` are two private copies of the same editor deploy (same `HorizonEditor` and `libHorizonCore` hashes: `df3a7e025082`, `74de8c836d13`); only the four `M_AutoLandscape*.hasset` differ. `new` carries the committed step-2 materials (hash equal to `git show cfa5bcb1:...`), `old` those of `cc1b3528`, the last commit before step 2 (old material into the new engine is the safe direction; no `Cannot load asset` or shader-compile error in any of the 48 logs). Provenance: `docs/perf-audit/raw-snow-sand-step1/binaries.txt`.
* **Scene.** `gen_autoland_scene.py` landscape, 256 m, rolling relief, no layer meshes (so the terrain material is the only thing that changes), one terrain per project and group (`scripts/perf/snow_sand_setup.py`), the group's own material id (Mountains `0x412`, Forest `0x415`, Grassland `0x419`, Desert `0x4A0`). Two poses: **top** = 150 m up, looking down (terrain fills the frame: the worst case for per-pixel cost) and **low** = 15 m up, nearly level (gameplay height; far terrain is small).
* **Method.** `landscape_regression.py --detailed --scale 2` (RenderScale 2 = 4x the pixels so the GPU never idles and a pass p50 is the cost, see the Thema 180 audit, "Die Falle bei der GPU-Zeit"), 200 frames warm-up, 600 recorded, 3 repetitions per cell, old/new order alternating every repetition, driver in `scripts/perf/snow_sand_cost_ab.py`. Display held awake with `keepawake.sh`; no compiler, linker or second editor ran (`foreign: none` in every `conditions.txt`). The Mac was on a locked screen (`locked: Yes`, as in the earlier audits), which is why the Tonemap pass is reported as the clock witness: 0.34 to 0.42 ms in every row, i.e. the same clock.
* **Metric.** Scene-pass GPU p50 (the landscape is drawn there) and the sum of all pass p50s, from `gpuPassPercentiles`. The whole-frame `gpuMs` is not used (it contains waiting).
* **Backend.** Metal only (Apple GPU, tile-deferred). Raw data: `docs/perf-audit/raw-snow-sand-step1/` (`tables.md`, 48 x summary/scopes/conditions).

### 2.2 Result (Metal, median of 3, ms)

| pose | group | Scene p50 old | Scene p50 new | delta | sum of passes old / new | Scene min..max old / new | Tonemap old / new |
|---|---|---|---|---|---|---|---|
| top | Mountains | 15.09 | 12.22 | **-2.87 (-19.0 %)** | 18.02 / 15.17 | 14.94..15.40 / 12.20..12.44 | 0.35 / 0.35 |
| top | Forest | 16.53 | 13.77 | **-2.76 (-16.7 %)** | 19.55 / 16.79 | 16.47..17.16 / 13.71..14.09 | 0.41 / 0.40 |
| top | Grassland | 15.06 | 12.35 | **-2.71 (-18.0 %)** | 18.12 / 15.41 | 15.05..15.75 / 12.24..12.43 | 0.42 / 0.42 |
| top | Desert | 14.41 | 11.63 | **-2.78 (-19.3 %)** | 17.46 / 14.91 | 14.32..14.88 / 11.48..11.97 | 0.41 / 0.34 |
| low | Mountains | 13.70 | 12.32 | **-1.38 (-10.1 %)** | 16.49 / 15.13 | 13.66..13.94 / 12.31..12.51 | 0.37 / 0.37 |
| low | Forest | 12.52 | 12.16 | -0.36 (-2.9 %) | 15.37 / 15.19 | 12.52..12.55 / 12.12..12.17 | 0.37 / 0.39 |
| low | Grassland | 12.17 | 12.04 | -0.13 (-1.0 %) | 15.24 / 15.34 | 11.80..12.20 / 11.78..12.25 | 0.39 / 0.42 |
| low | Desert | 11.83 | 10.88 | **-0.95 (-8.0 %)** | 14.88 / 14.36 | 11.80..11.98 / 10.78..11.31 | 0.39 / 0.39 |

How to read it:

* **top**: all four groups improve by 2.7 to 2.9 ms (the ranges of old and new do not overlap). The delta is the same in every group although Forest, Grassland and Desert ran on 128 px placeholder arrays and Mountains on the real 2K arrays, so the saving is the removed work itself and not texture bandwidth.
* **low**: smaller, because far terrain covers few pixels. Mountains (real 2K arrays) -1.4 ms and Desert -1.0 ms are outside the spread; Forest -0.4 ms has non-overlapping ranges but a gap of only 0.35 ms between them; **Grassland (-0.13 ms) is inside the spread and counts as zero change**.
* **Why it goes down.** The baked fragment text loses texture calls: Mountains / Forest / Grassland 174 -> 138 (the snow role's 9 taps in three arrays), Desert 174 -> 102 (snow and the sand role). The old snow taps were gated by the snow line in the graph, yet removing them saves 2.7 ms where no snow is visible, so the gate did not make them free on this GPU (why was not investigated: a gated sample may still be executed for the derivatives, or the branch may be flattened). The new noise (2 `FbmGrad` for snow, `FbmGrad` + `Fbm` + 3 sines for sand) costs less than the taps it replaces. The call count is static text, not a profile.

### 2.3 What this does and does not say

* It is the **net** change of step 2 on one GPU. The gross cost of the two functions alone cannot be read from it (that would need a variant without them, which means editing material builders; not done here). What can be said: the cost of both functions is smaller than the cost of the texture reads they replaced, on Metal, in this scene.
* The scene has no snow above the snow line, but the old gate did not skip the work, and the new functions run on every pixel by construction, so the visible amount of snow does not move either side.
* Not measured: D3D11, D3D12, Vulkan, OpenGL (see section 5). Other GPUs weigh ALU against bandwidth differently; the sign on a desktop card with a big texture cache is not known. NN-WS03 (RTX 4070) is the place to repeat `snow_sand_cost_ab.py` with `--rhi D3D11` / `D3D12` / `Vulkan` (the script and the setup are platform-neutral except the `cp -R` deploy copy and `keepawake.sh`).
* The trail read (section 3) adds about 4 integer fetches in the common case (L0 only), against the ~36 taps the material still makes per pixel. It is expected to cost a few per cent of the Scene pass; **this is an estimate, not a measurement**, and step 5 has to measure it with the same driver.
* **Consequence for step 2 of the topic ("reduce the cost of the functions, if the measurement demands it"): it does not.** On Metal there is nothing to reduce. Step 2 is only needed if the D3D/Vulkan repeat shows a regression.

## 3. Trail data model

### 3.1 What the engine gives us (verified in code)

* `MF_ProceduralSnow` pin 9 and `MF_ProceduralSand` pin 8, `Disturbance` (Float, 0..1), are **declared and read by nothing** (`ProceduralSurfaceFunctions.h:27`, the builders end `f.in("Disturbance", ...)`). The landscape wires nothing to it (`AutoLandscapeMaterial.cpp:195-204` connects Position, Cover, Color, Melt only). The assignment text said Disturbance "already receives" a weather signal; it does not. Nothing needs to be unwired, and nothing delivers a value yet.
* Weather inputs that can heal a trail are on `WeatherComponent` (`WeatherComponent.h:40-75`): `precip` (0..1) with `precipType`, `windSpeed`, `wetness` (the landscape feeds it to snow `Melt` and sand `Wetness`). **`snowCover` is a hand-set slider, not accumulated snowfall**, so "new snow heals" must key on `precip` while `precipType` is snow, not on `d(snowCover)/dt`. There is no `freshness` value.
* The landscape's weightmap is a per-terrain texture (`TerrainComponent::weightRes`, 1..4096, `TerrainPaint.cpp:21`), bound per draw (`heLandscapeWeights`: GL unit 13, Vulkan binding 14, D3D11/D3D12 `t14` + `s0` linear clamp). Weights are paint data at paint resolution; a footprint (0.3 to 0.5 m) does not fit in it, so trails cannot ride in a fourth weight page.
* Sampler budget is the hard constraint. GL already assigns units 0 to 20 (`OpenGLRenderer.cpp:4005-4045`; macOS GL 4.1 allows 16 sampler uniforms in one fragment shader, so what is really sampled by the landscape shader is the number that counts, not the unit index). D3D11/D3D12 SM 5.0 is out of sampler registers (`MaterialShaderLibrary.cpp:3108-3156`, "frees the SM 5.0 sampler register heLandscapeWeights sits on"). Vulkan's fragment stage is at 17 combined samplers and has a precedent for a material that "has no sampler slot left" and degrades (`VulkanRenderer.cpp:2947-2969`, `3271-3300`).

### 3.2 Decision table

| Topic | Decision | Why | Risk |
|---|---|---|---|
| Where the field lives for the shader | **Two-level clipmap in one 2D array texture `heTrails`**, 2 layers of 512 x 512, centred on the camera (snapped to whole texels, toroidal wrap): L0 covers 128 m at 0.25 m per texel, L1 covers 512 m at 1 m | Footprints and tyre tracks are sub-metre; they are only visible close (the functions already fade 6 cm grain out by 27 m: `fadeFar`, cell x 150..450). L1 keeps long tracks readable to 250 m as soft grooves. One binding, 2 MB | A second landscape in view shares the same world-space field (good: trails belong to the world, not to a terrain) |
| GPU format | `R32_FLOAT` (depth, 0 = untouched, 1 = fully pressed) | The only typed-UAV format every D3D11/D3D12 feature-level-11 device must support for store; float accumulates a slow heal without sticking | 2 MB is nothing; if a smaller format is needed later `R16_FLOAT` is a drop-in |
| How the shader reads it | **Integer fetch** (`texelFetch` / `Load` / `read`) of the 4 neighbouring texels and bilinear by hand; the gradient comes from the same four texels | Needs a texture slot but **no sampler**, so SM 5.0 is not over budget; the 4 texels give depth and slope (needed for the rim normal) at once | Still one texture unit: on GL (macOS 16 samplers) it may not fit. Fallback = the Vulkan precedent: compile the read out (`HE_TRAILS` define false) and log it; to be checked by counting live samplers in the linked landscape program (section 7) |
| Free slots (for the one binding) | D3D11/D3D12: `t19` (DDGI atlases are `t17/t18`); Vulkan: binding 15 as a **separate** sampled image (not combined), which is not limited by the sampler count; Metal: next free `texture(n)` (limit 128); GL: unit 21 if the sampler count allows | from the binding comments quoted in 3.1 | to be verified per backend when step 5 builds it; D3D/Vulkan cannot be verified on this Mac |
| Write path, compute backends | **Metal, D3D12, Vulkan, D3D11: one compute dispatch per frame** that (1) scrolls/clears the rows the clipmap moved over, (2) applies the frame's stamps, (3) applies the heal tick. D3D11 has compute shaders (CS 5.0) on every feature-level-11 device | Same kernel text four times, one dispatch of 512 x 512 x 2 = 0.5 M threads, far below measurable | D3D11 typed UAV store of R32_FLOAT is mandatory, so no optional-format query is needed |
| Write path, GL 4.1 | **Render pass into the array layers through an FBO**: one instanced quad per stamp with `glBlendEquation(GL_MAX)` for the press, a full-screen pass per layer for scroll and heal (`R32F` render target, float blend) | GL 4.1 has no compute shaders and no image load/store (both 4.2/4.3) | float blending to `R32F` on the macOS GL-over-Metal driver is untested; this Mac can test it, so step 5 checks it first. If it fails, `R16F` is the fallback |
| Stamps | CPU builds a small array per frame (`pos.xz`, `radius`, `depth`, `kind`, `yaw`, `shape`), at most 256 per frame, uploaded as a constant/structured buffer; analytic ellipse and capsule shapes, no stamp textures in v1 | Footprint = ellipse (two per stride), tyre = capsule between the last two wheel positions so fast vehicles leave a line, not dots | A stamp texture (boots with a tread) is an art request, not a data-model change |
| Authoritative store | **CPU, sparse pages**: world-aligned 128 m x 128 m pages (`pageX = floor(x / 128)`), each 512 x 512 x `R8_UNORM` (256 KB dense, compressed to a few KB when mostly zero), allocated on the first stamp. The GPU clipmap is a cache that the page store refills as the camera moves | The gameplay side (audio by surface depth, tracking) can query a texel without a GPU readback; save files and streaming move pages, not textures | 8 bit at 0.25 m resolves 4 mm of depth over a 1 m range, enough; the heal tick must not truncate (3.4) |
| Resident budget | GPU: 2 MB fixed. CPU: dense pages in an LRU capped at **256 pages = 64 MB**; beyond that a page is kept only in its compressed form (zstd, typically 1 to 20 KB). Hard ceiling on compressed pages per save: 16 384 (about 128 MB worst case, 16 km radius fully walked, which no play session produces) | A 100 km world at 0.25 m dense would be 4 TB; only trodden pages exist | the 64 MB cap is a proposal to confirm with the human (section 6, Q8) |

### 3.3 How Disturbance reaches the functions

Today `Disturbance` is one Float. The rim normal needs the slope of the field, which one scalar cannot carry (screen-space `dFdx` of a fetched value is quad-based and wrong at edges). So step 5 changes the interface by **appending** one input, so no existing pin index moves and the landscape graph keeps working untouched:

| Function | Existing | Appended in step 5 |
|---|---|---|
| `MF_ProceduralSnow` | `Disturbance` (pin 9, Float) | `Disturbance Slope` (pin 10, Vec2, default 0): d(depth)/dx, d(depth)/dz of the field per metre |
| `MF_ProceduralSand` | `Disturbance` (pin 8, Float) | `Disturbance Slope` (pin 9, Vec2, default 0) |

`kSnowInCount` / `kSandInCount` and the interface tests (`fnInputsReaching`, the shipped-function tripwire) move with it, and both `matfn_gen` and `landscape_tex_gen --material` (all four groups) must be re-run because the function bodies are inlined into the baked landscape GLSL. Because of the node-order lesson of 329 step 7, any new builder code must put at most one node-creating call into an argument list.

What the function does with `D` (depth 0..1) and `S` (slope), where `eff = D * cover` for snow (a thin cover cannot hold a deep print) and `eff = D` for sand:

| Output | Effect | Reason |
|---|---|---|
| Normal | `n = normalize(n_surface - k_n * eff * vec3(S.x, 0, S.y) )` on top of the existing `bendNormal`; the pit floor is flat (S = 0), the rim is steep | gives the rim light that makes a print readable at grazing sun |
| Albedo | snow: x `(1 - 0.18 * eff)` and a slight blue shift (compacted snow); sand: x `(1 - 0.25 * eff)` (damp, compacted) | compaction darkens |
| Roughness | snow: `- 0.25 * eff` (glossy pack); sand: `- 0.15 * eff` | |
| AO | `- 0.35 * eff` in the pit, so a deep print shades itself | |
| Height | **unchanged** | see 3.5 |

### 3.4 Healing

The heal runs on the GPU in the same dispatch as the stamps, at a fixed tick (default 4 Hz of **game** time) so that a step is never smaller than the 8-bit persistence can hold. Per texel, with `dt` the tick length:

```
// inputs from WeatherComponent / EnvironmentComponent (already on the CPU every tick)
snowFall  = (precipType == Snow) ? precip : 0           // 0..1, new snow
wind      = clamp((windSpeed - 1) / 4, 0, 1)            // windSpeed 1 = calm
melt      = wetness                                      // the same value the landscape gives the functions
rain      = (precipType == Rain) ? precip : 0

// snow surface: new snow fills the print, wind drifts it shut, thaw rounds it off and slumps it
healSnow  = hSnow * snowFall + hDrift * wind * driftExposure + hMelt * melt
// sand surface: wind refills, rain crusts and FREEZES the print (wet sand keeps its shape)
healSand  = hWind * wind * wind * (1 - 0.9 * wetness) - 0.0        // quadratic: only a real wind moves sand
healSand *= (1 - 0.9 * rain)

D_new = max(0, D - heal * dt)                            // GPU float, no truncation
```

Defaults (all authored in a `TrailSettings` block, step 5): `hSnow = 1/120 per s` (a full print fills in 2 minutes of steady snowfall), `hDrift = 1/1800` (30 minutes in a gale), `hMelt = 1/600` (10 minutes at full thaw), `hWind = 1/900`. `driftExposure` is 1 in v1; a later step can read the drift field of `MF_ProceduralSnow` (ridges drift shut first, lees stay) without changing the data model. Which of the two formulas applies is decided **per page** in v1: a page carries a one-byte surface mask (bit 0 snow, bit 1 sand), set by the first stamp of that kind, and the heal uses the larger rate of the kinds present. That is wrong only on a page that really holds snow and sand side by side (a mixed-group terrain at the boundary of two groups); a per-texel kind would need a second plane or a weightmap fetch in the heal kernel, and is not worth it before somebody sees the artefact.

Why not "decrease while the cover slider rises": `WeatherComponent::snowCover` is set by hand and presets do not move it, so its derivative is zero in normal play.

**Pages that are not resident** do not tick. Every page stores `lastTickTime` (world seconds). When it loads back, the CPU applies the closed form for the elapsed time with the **current** weather rates, `D * exp(-lambda * dt)` for the proportional parts and `max(0, D - rate * dt)` for the constant ones, capped at 24 game hours, and the step then writes the corrected page. This is an approximation (the weather in between is unknown) and is written down as such; a savegame that stores the weather history can do better later.

### 3.5 Disturbance and Height (the trap in this design)

`MF_ProceduralSnow.Height` was made weather-free on purpose: the landscape reads it into `r.snowMask` (`AutoLandscapeMaterial.cpp:330-335`), and the structural cone test plus the Mountains md5 witness (`819b347a...`) guard it. If `D` entered `Height`, the paint-layer blend would start to depend on live gameplay state through a texture, the structural test (which looks for Weather nodes in the cone) would not notice, and the witness would only hold on a scene without trails. **Decision: `D` and `S` do not enter `Height`.** A deep print therefore never shows the dirt layer through the snow in v1; that is Q2 in section 6, and if the human wants it, it is a separate, explicit input of the layer-blend node (`Reveal`), not a side effect of Height.

### 3.6 Persistence with the tiles of Topic 326

| Aspect | Decision | Why |
|---|---|---|
| Key | `(pageX, pageZ)` in world metres / 128, **not** relative to a terrain or a tile | 326's tile size is not decided (plan: 64 cells x 2 m = 128 m patches at the finest level). A page that equals the finest patch is stored with it; if 326 chooses larger tiles, a tile holds a 2D list of pages. Either way no trail data moves when a tile does |
| Where it is written | Into the **streaming tile record as an optional tail chunk** `TRAL` (list of `(pageIndex, lastTickTime, zstd(R8 512x512))`). `HAsset.h` says a new tail chunk needs no file-version bump, and an old reader skips it. A test must load an old tile without the chunk | no format break |
| Before 326 exists | The single landscape stores the same chunk on the `TerrainComponent` in the **save game / world cache, not in the `.hescene`** | trails are runtime state of a play session, not authored content; writing them into a scene asset would change the file every time somebody walks over snow in the editor. An authored "trail brush" (paint footprints by hand) is a separate question (Q5) |
| Load | Pages come in with their tile; the clipmap refills from the page store as the camera moves (CPU copies the new rows into a staging buffer and the GPU scroll pass consumes it, at most 64 KB per frame) | no stall |
| Unload | A dirty page is compressed and handed back to its tile before the tile is evicted; a clean page is dropped | |
| Multiplayer | Not replicated in v1. Stamps are deterministic events and could be replicated later instead of the field | out of scope |

## 4. How the later steps depend on this

| Step | Needs from this document |
|---|---|
| 2 Cost reduction | only if section 2 shows a cost worth cutting; the measurement says whether |
| 3 Parameters | the 13 landscape parameters (`kAutoLandscapeParamCount`) grow; trail settings are CPU-side (`TrailSettings`), not material parameters, so they do not enter that count |
| 4 Mixing texture and procedural | an input `TextureMix` per role; independent of the trail field. The trail read must stay behind the procedural function so a role that is texture-only simply has no trails (Q3) |
| 5 Field and stamps | 3.2, 3.3, 3.4; first check the GL `R32F` blend and the live sampler count on this Mac |
| 6 Persistence | 3.6; needs 326 step 1 for the page key |
| 7 Gameplay | `HE::api::stampTrail(...)` and `HE::api::sampleTrail(x, z)`; footprint audio reads `sampleTrail` |

## 5. What this step did not do

* No D3D11, D3D12 or Vulkan measurement: this was done on the Mac only (Metal). The first attempt on NN-WS03 got as far as starting a configure (it was still downloading the Steam Audio SDK) and generating scenes, and was stopped before any frame was measured, so there is no D3D number in this document.
* No OpenGL measurement: `he_perf_capture.py --rhi OpenGL` timed out in the Thema 180 audit and was not tried again.
* The three groups without real arrays on a developer machine (Forest, Grassland, Desert) were measured with the 128-pixel placeholder arrays. See the caveat under the table.
* The CI of the fixed 329 branch is pending.

## 6. Open questions for the human

The first post of Thema 328 (landscape-planner) could not be read in full: `hive_read_topic` truncates it, it is not in the local search index, and request #159 is unanswered. Its first open question is quoted below (Q1); the rest are the questions this design raises. If the original post lists more, they have to be added.

| # | Question | Proposed answer | Why / risk |
|---|---|---|---|
| Q1 (post) | Should trails be visual only, or also gameplay (footprint audio, reading and following tracks)? | Visual first; the CPU page store is queryable (`sampleTrail`), so audio and tracking need no GPU readback and can come in step 7 | the design already pays for it (CPU is the source of truth); costs nothing extra now |
| Q2 | Should a deep print show the layer under the snow (bare dirt), or only compress the snow? | Compress only (v1) | showing the layer would put live state into `r.snowMask` (3.5); doing it later is a new input, not a rewrite |
| Q3 | Texture-only roles (step 4, e.g. a rock layer with snow on top): should they take trails? | Only the procedural part (the snow or sand overlay) carries trails | the trail read lives inside the two functions |
| Q4 | How fast should trails heal: realistic hours or gameplay minutes? | Gameplay minutes by default (a print survives about 2 min of steady snowfall, 30 min of gale, 10 min of thaw), all rates authored in `TrailSettings` | realistic hours make a print permanent for a play session |
| Q5 | Are trails saved with the game only, or may the editor author them (a brush) and write them into the scene? | Save game / world cache only in v1; a hand-painted trail brush is a separate step | otherwise every editor walk dirties the scene file |
| Q6 | Who leaves trails: the player and vehicles, or any entity (animals, NPCs)? | Anything with a `TrailEmitter`; the player and vehicles are two presets | the budget is 256 stamps per frame; a herd stays inside it |
| Q7 | Do trails need to replicate in multiplayer? | No in v1 | stamps are events and can be replicated later |
| Q8 | Memory ceiling: 2 MB GPU, 64 MB dense pages in RAM, 16 384 compressed pages per save? | Yes, as proposed | all three are constants of `TrailSettings` |
| Q9 | Footprint shape: analytic ellipses, or a stamp texture with a tread pattern? | Analytic first | a texture is a content request on top, not a data-model change |
| Q10 | Winter season (Thema 330) and trails: does the winter state raise the default snow cover? | Trails follow the weather values only; seasons change textures, not heal rates | keeps 328 independent of 330 |

## Handoff

* **Topic / step:** 328, step 1 of 8 (Design und Messbasis). Docs and measurement only.
* **Done:** prerequisite check (section 1) including the CI finding and its cause; Metal cost A/B per group (section 2); trail data model, write paths, Disturbance wiring, healing, persistence (section 3); open questions (section 6). Scripts added: `scripts/perf/snow_sand_setup.py`, `scripts/perf/snow_sand_cost_ab.py`.
* **Not done / not verified:** D3D11, D3D12, Vulkan and OpenGL costs; the GL 4.1 `R32F` max-blend write and the live sampler count of the landscape shader (both are step 5's first checks); the CI run of the fixed 329 branch; the full text of the first post of 328 (only Q1 is quoted, section 6).
* **Decisions taken here:** clipmap + sparse page store; integer fetch, no sampler; `D` and `S` do not enter `Height`; healing keys on `precip` and `windSpeed`, not on `snowCover`; trails live in the save/world cache, not in the `.hescene`.
* **Decisions only suggested:** all numbers in 3.2 and 3.4 (resolutions, caps, rates); appending `Disturbance Slope` to both functions.
* **Next steps, in order:** 1. merge or rebase on the 329 branch once its CI is green (nothing in 328 can be built without the functions); 2. answer Q1 to Q10 (or confirm the proposals); 3. step 2 only if section 2 says the cost needs cutting; 4. step 5 starts with the two checks above, on this Mac.
* **Traps:** a function change needs `matfn_gen` and `landscape_tex_gen --material` for all four groups (the bodies are inlined into the baked GLSL); at most one node-creating call per argument list in builder code; `snowCover` is a slider, not accumulated snow; do not copy files into another worktree's deploy (the cost runs used private copies under `/tmp/he328`); hold the display awake (`keepawake.sh`) for any Mac capture.
