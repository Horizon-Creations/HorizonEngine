# Landscape 3.0 Folge, step 2: procedural snow and sand as material functions (Thema 329)

The auto landscape's Snow role (all four groups) and the Desert group's ground role (Sand) are no
longer read from the texture arrays. Two flat engine functions make them from noise in world space, and
any other material can call them too.

## The functions

`HE_Core/include/MaterialGraph/ProceduralSurfaceFunctions.h` (interface, pin enums, ids) and
`src/MaterialGraph/ProceduralSurfaceFunctions.cpp` (the builders, the single source). Assets
`EditorDeps/EngineContent/MaterialFunctions/Surface/MF_ProceduralSnow|Sand.hasset` (matfn_gen), ids
`0x41D` / `0x41E` (the two left below the loose maps; `0x41F` stays free). Path
`Engine/MaterialFunctions/Surface/…`. `weatherFunctionLoader()` answers all four functions the landscape graph calls.

| | MF_ProceduralSnow | MF_ProceduralSand |
|---|---|---|
| In (all with a default) | Position (Vec2, world XZ m), Cover 1, Color 0.85, Grain 0.5, Drift 0.5, Wind Direction (Vec2), Glitter 0.35, Softness 0.5, Melt 0, Disturbance 0 | Position, Color 0.76, Grain Size 0.03 m, Ripple 0.5, Ripple Size 0.3 m, Wind Direction, Glitter 0.3, Wetness 0, Disturbance 0 |
| Out | Albedo, Normal (world), Roughness, AO, Height | the same five |
| Noise | 2 x FbmGrad (drift in the wind frame, 8 m x 1.8 m; grain at 6 cm) | 1 x FbmGrad (grain), 1 x Fbm (ripple wobble, 4 m), 3 sines |

* **Flat** (no FunctionCall inside), no Param node, no texture, **no Weather node**: Cover / Melt / Wetness are wired by the caller.
* **Disturbance** is declared (pin 9 / 8) and read by nothing: the hook for trails.
* **Height does not move with Cover, Softness, Melt, Wetness or Color** (it reads Position, Wind, Drift/Ripple, Grain only),
  so the landscape's height-snow mask stays weather-free. Both properties are tests (`fnInputsReaching`).
* Fine detail fades with the camera distance (`CameraDistance`, cell x 150..450) so a 6 cm grain does not shimmer.
* Ripple phase goes through `fract()` before the sine (a km from the origin it is 10^4..10^5 rad).
* Wind: `Wind Direction` is a Vec2, a zero vector falls back to +X (no NaN). The landscape leaves it at the default
  on purpose: wired to the live Wind node every ripple would rotate about the world origin when the wind turns.

## Wiring in `buildAutoLandscapeGraph`

* `AutoLandscapeGroup::proceduralSand` (Desert only): role 0 comes from `MF_ProceduralSand` instead of the Grass slice.
  Colour = mean of the reference (coast_sand_01 4k diffuse, sRGB 129/114/91 = linear 0.221/0.168/0.104), Wetness = Weather.wetness.
  Slice 0 of the Desert arrays is not read any more.
* Snow, every group: `MF_ProceduralSnow` with Position = world XZ, Cover = **the Weather node's Snow Cover pin** (not
  MF_WeatherSnow's Cover output: that call takes the snow's own height bias as an input, which would make the call node depend on
  itself), Melt = Weather.wetness, Color constant (0.82, 0.85, 0.90). Slice 3 is not read.
* The function outputs enter the existing blends as `Reroute` (albedo, normal; one output pin per node) and `Combine3(AO, roughness,
  height)` (the arrays' mask layout), so `blend()`, the height biases and the paint-layer mix are untouched.
* **The snow gate is gone**: a function call is never moved into an If branch (`branchPure` has no FunctionCall, and function
  bodies are not guarded at all), so the `If` would have been a `mix()`. The noise now runs on every pixel (see Not verified).
* `r.snowMask` is the same formula; its height bias reads the function's Height output. The old test "no FunctionCall in the
  cone" is now pin-aware: no Weather node and no weather function in the cone, and of the snow call only the pins Height reads count.

## Verified (Mac, Release, tree of this commit)

* Full `ctest -j4` (empty HOME) 345/345 on the final tree (the first full run caught `test_auto_landscape_variants`, which counted four bombed layers: now 27 reads). `he_tests --source-file=*test_material_graph.cpp` 109/109 (15 618 assertions), `*test_landscape_groups.cpp` 4/4. New cases: interface
  and flatness, an unwired call compiles on its own (no weather, no texture, integer-hash noise only), the shipped-function tripwire
  now covers `Surface/` and both M_AutoLandscape and the Desert material, the hand-built graph calling both functions and the Desert
  material in the node sweeps (HLSL text 93/93, SPIR-V 93/93, GLSL 4.30 clustered 93/93); the shipped-material bake test (`test_landscape_groups`) bakes all four
  materials for every backend incl. MSL, and Metal / OpenGL ran them for real in the witness. FXC / D3D12 / Vulkan validation of the new cases only exists on other devices: not run.
* `matfn_gen` rewrites every UI effect and weather function byte-identical; the four `M_AutoLandscape*.hasset` are regenerated with
  `landscape_tex_gen EditorDeps/EngineContent/Materials --material [--group G]`, the deploy equals the committed file.
* Witness `scripts/landscape-3-0-accept/cap233.sh` (top-down 400 m, real Mountains arrays from the cache, Desert placeholder arrays,
  GI/SSAO/AA/bloom off, `missing-logs=0`, counters unchanged 4 draws / 34 816 tris):

| Witness | before (step 1 deploy) | after |
|---|---|---|
| Mountains Metal | `5c591e66375d49a41919bc4e5405a534` | `819b347a055a2e65754096f4f4e28c43` (twice, identical) |
| Mountains OpenGL | `ac06582d97fcafe699167c9d6972419f` | `6d40e52737cdfe443f32ef6da8aca3e9` |
| Desert Metal / OpenGL (top) | n.a. (no baseline kept) | `3d9a3c15…` / `93854baf…` |
| Close snow (`CAMX=30 CAMY=346 PITCH=-30 TOD=0.3`) Metal vs OpenGL | | mean abs diff 0.0/255, max 1 |

  The Mountains baseline is **new** by design (the human allowed it): the hex-tiled snow texture is gone, the snow is smooth with
  wind-drift streaks (`mountains-top-before.jpg` / `-after.jpg`, `snow-close-metal.jpg`, `sand-close-metal.jpg` here). Regression
  guard from now on: the after md5s above, with the same camera and `cap233.sh`.

## Not verified / open

* **Cost.** Every landscape pixel now runs 2 FbmGrad (snow) and, in the Desert, FbmGrad + Fbm + 3 sines (sand) where the old snow
  read 9 gated taps only above the snow line. Not measured (the machine was shared; `scripts/perf/autoland_matrix.py` /
  `landscape_regression.py` with the old deploy are the tools). The cure would be codegen: let a flat pure function body move into an
  If branch (`computeBranchGuards`, `placeDecl` guard child scopes).
* D3D11 / D3D12 / Vulkan not run (no device); the text goes through the HLSL, SPIR-V and GL cross-compile sweeps above.
* Sand look tuned against the reference only for mean colour and speckle contrast (std 13/255 in the photo), by eye in two Metal /
  OpenGL oblique frames; never looked at in the live editor, never with real Desert arrays.
* GI bounce kernels still read slice 3 / slice 0 of the arrays for their layer colour (their own approximation, unchanged).
* `heightBias` / slope ramp were **not** extracted into functions: nothing gained (a function call is impure for the branch
  guards and adds inlined interface copies to four baked shaders) and a bit-identity claim would have been confounded by the
  re-baseline in this very step.

## Handoff

- **Topic / step:** 329, step 2 of 6
- **Branch / commit:** `claude/landscape-3-0-folge-path-layer-prozeduraler-schnee-sand-mate`, code @ 59c92753 + the tuning commit that follows it (pushed: see the hive post)
- **Done:** snow (all groups) and Desert sand are `MF_ProceduralSnow` / `MF_ProceduralSand`; reusable by any material; r.snowMask unchanged in formula.
- **Files touched:** `ProceduralSurfaceFunctions.h/.cpp` (new), `MatFnBuilder.h` (new, shared builder; Weather builders use it, assets byte-identical),
  `WeatherMaterialFunctions.cpp:~120` (loader answers 4 paths), `AutoLandscapeMaterial.cpp:~140-215` (reads), `AutoLandscapeMaterial.h` (`proceduralSand`),
  `HE_Core/CMakeLists.txt` (explicit source list!), both generator mains, `tests/test_material_graph.cpp` (cone test ~l.3990, tripwire, cross-compile list), `tests/test_landscape_groups.cpp`.
- **Verified / Not verified:** see above.
- **Decisions taken:** Cover from the Weather node, not MF_WeatherSnow (node-level cycle); Height weather-free instead of routing the bias around
  snowMask; no snow gate; wind fixed in the landscape; colours are constants in the graph, **no new exposed parameters** (the count stays 13);
  new folder `Surface/`; `weatherFunctionLoader()` extended instead of a second loader.
- **Decisions suggested, not taken:** expose snow/sand colour and drift as landscape parameters (changes `kAutoLandscapeParamCount` and the instance panel);
  branch-guard flat function bodies in codegen (perf); a wind-driven (not origin-rotating) ripple direction.
- **Pending:** perf A/B of the landscape pass; EngineContent upload of the two new functions and four materials (outward-facing, not done); D3D/Vulkan look.
- **Traps:** a function tuning change means `matfn_gen` AND `landscape_tex_gen --material` for all four groups (the baked text inlines the function body);
  editor deploy gets `EngineContent` at editor build only; `HE_Core` sources are listed by hand in `src/HE_Core/CMakeLists.txt`; the he_tests build overwrites
  `out/deploy/Editor` libs, so keep a copy before the next A/B (`scripts/perf/selfcontain_deploy_copy.sh`).
- **Next steps:** 1. step 3 (Path layer) can take layers 8..11, nothing here blocks it; 2. a perf A/B (old deploy vs new) before the human ships this;
  3. tune the look in the live editor with real Desert arrays (sand colour constant in `AutoLandscapeMaterial.cpp`, ripple/grain numbers in the builders).

## Step 7: CI regression (Linux/Windows `nodeGraphJson == materialGraphToJson` red)

- **Cause:** `ProceduralSurfaceFunctions.cpp` built nodes as nested sibling arguments (`add(mul(a,b), mul(c,d))`). C++ leaves the order of a call's arguments unspecified, so Clang (the Mac that committed the `.hasset`) numbered the nodes differently from GCC/MSVC. Same shader, other node ids: CI's two JSONs (run 40879630317) had 155 nodes, the same node-type multiset, 46 differing ids and different links.
- **Fix (d27fad34):** every op call has at most one node-creating call among its arguments; sibling nodes are named locals, built left to right (the order Clang already used). `matfn_gen` therefore reproduces the committed `MF_ProceduralSnow/Sand.hasset` byte for byte, and the baked landscape materials need no regeneration.
- **Verified on the Mac:** `matfn_gen` leaves `git status` clean, `test_material_graph` green, full ctest 345/345 (`test_hpak` only with a private `HOME`, otherwise it counts the real EngineContent cache). A source scan finds no call with two node-creating arguments (it flagged 38 in the old file). **Not verified:** GCC/MSVC locally, CI run pending.
- **Rule for new builder code:** never put two `o.*` node-creating calls (or `fadeFar`/`bendNormal`) in the same argument list; assign each to a named local first. A `{}` initializer list is ordered and is fine.

## Step 3 (Thema 328): look as landscape parameters

- **What:** `Snow Color` (Color picker), `Snow Look` (Vec4: Grain, Drift, Glitter, Softness) in every group, `Sand Look` (Vec4: Grain Size m, Ripple, Ripple Size m, Glitter) in the Desert. The wiring goes Param -> Split RGBA -> the function's pins (`AutoLandscapeMaterial.cpp`, param block after `pPudSlope`, wiring at the sand and snow calls). Defaults are the functions' own, colours as before. Melt/Wetness stay driven by the weather.
- **Why packed:** a graph has `kMatMaxParams` = 16 HeParams slots (hard-coded 256-byte upload in all five renderers, `test_engine_materials`), 13 were taken. Packing gives 15 per group, Desert 16 (budget full). Sand colour therefore stays baked (reference mean, sRGB 129/114/91). `autoLandscapeParamCount(group)` is the new count; `kAutoLandscapeParamCount` = 15 is the common part.
- **Assets:** all four `M_AutoLandscape*.hasset` regenerated (`landscape_tex_gen ... --material [--group G]`), 221/221/221/218 nodes, 15/15/15/16 params.
- **Verified (Mac, Release, Metal):** full ctest 345/345 (private HOME). New test "Procedural snow and sand: the look is an exposed parameter" (kinds, defaults, no slot baked for budget). Witness A/B before/after (`cap233.sh`, Mountains top + snow plateau close, Desert top + sand close): max difference 1/255, mean < 2e-4 (float rounding of the defaults), Mountains top bit-identical. The pin order is distinct in the defaults (0.5/0.5/0.35/0.5 and 0.03/0.5/0.3/0.3), so a mixed-up pin would show.
- **Not verified:** a changed value in the live editor (instance panel / Vec4 widget), OpenGL/D3D/Vulkan, a Material Instance override of the new names.
- **Shots:** `docs/landscape-3-0-procedural-snow-sand-shots/` (snow plateau, Desert sand ripples).

## Handoff (steps 3, 5a; superseded)

Superseded by the final handoff at the end of this document.

## Step 5a (Thema 328): trail field on the CPU, print look in the functions

Step 5 of the topic is split: this part is everything that can be built and verified on the Mac without a new texture binding in five renderers; the GPU upload and the landscape wiring are step 5b (below).

- **Functions:** `MF_ProceduralSnow` / `MF_ProceduralSand` got one appended input, `Disturbance Slope` (Vec2, pin 10 / 9, default 0), and now read `Disturbance` (pin 9 / 8). A print is darker (snow x(1-0.18 D), sand x(1-0.25 D)), glossier (-0.25 / -0.15 D roughness), shaded (-0.35 D AO), its rim leans the normal by 0.3 x slope, and in the sand it stamps the ripples flat (ripple slope x (1-D)). **Height is untouched** (the interface test now asserts neither input reaches Height). Both defaults 0 = the surface exactly as before.
- **`HE::TrailField`** (`src/HE_Core/include/Trails/TrailField.h`, `src/Trails/TrailField.cpp`): sparse pages of 128 m x 128 m at 0.25 m, 16-bit depth, allocated on the first stamp and freed when empty. `stampEllipse` (footprint, yaw), `stampCapsule` (tyre / sled line, spans pages), `tick(time, TrailWeather, TrailSettings)` (fixed 4 Hz steps, the plan's heal formulas: snow = hSnow x snowfall + hDrift x wind + hMelt x wetness, sand = hWind x wind^2 x (1 - 0.9 wet) x (1 - 0.9 rain), a page uses the faster of its kinds), `sample` (bilinear) and `gradient` (what `Disturbance Slope` takes). `tests/test_trail_field.cpp` (9 cases): soft press, max semantics, yaw, capsule across page borders, gradient sign, no-weather = no heal, snowfall rate, slow heal not truncating, sand wind^2 and wet/rain freezing, long pause.
- **Decisions changed against the plan (3.2/3.3/3.4):** (1) pages are **16 bit**, not R8: the plan's 8-bit store needs a float heal on the GPU to avoid truncation; the heal now runs on the CPU, so the extra byte buys it; (2) `eff = D` for snow, not `D x cover`: the snow mask already decides where snow lies, and `Cover` is 0 on height-snow, which would never take a print; (3) the field is **not** a GPU compute/FBO path (see 5b).
- **Verified (Mac, Release, Metal):** ctest 346/346. Witness A/B of the default render (D = 0) against the step 3 baseline: Mountains top bit-identical, snow plateau and sand close max 1/255 (same md5s as step 3: e7384f1d..., ff7b31d1...). `matfn_gen` + `landscape_tex_gen` regenerate every asset deterministically. A temporary experiment (functions rebuilt with a constant D = 0.8 and slope 1.2, deployed functions and materials swapped, restored byte-identical) changed the render hard (snow dark blue, sand black from the leaned normals), so the pins do reach the lighting; a constant is not a footprint, no screenshot is kept.
- **Trap found:** the editor reads the function assets from its deploy's `EngineContent/MaterialFunctions`, not only the baked landscape text: a function default changed in a test must be swapped there too.
- **Not done / not verified:** no footprint is visible yet; nothing feeds the pins. No GPU code, no landscape wiring, no D3D / Vulkan / OpenGL. The CPU heal is a 512 x 512 pass per page per tick (fine for tens of pages, not measured).

## Step 5b (Thema 328): trails reach the screen

Footprints and tracks are now visible in snow and sand on Metal and OpenGL. **No renderer code changed**: the clipmap is an ordinary texture asset in the landscape material's free fourth graph-texture slot (heTexP, resolved by every backend for the three arrays already).

- **`HE::TrailClipmap`** (`Trails/TrailClipmap.h/.cpp`): a 128 m x 128 m window around the camera as a 512 x 512 RGBA8 texture, path `mem://HorizonTrails` (no file, so the EngineContent resolver never looks for it), `srgb = false`, one mip. World-aligned and toroidal: the texel of (x, z) is (floor(x/0.25) mod 512, floor(z/0.25) mod 512), the shader uv is `xz / 128` with the sampler's repeat wrap, so **no window origin travels to the GPU** (Desert is at 16/16 parameters). R = 255 - depth8 in this step (white = untouched; 5c changed it to a signed value, see there), G / B = slope x / z (128 = flat, +-8 per metre), A = 128 marker. The CPU computes the slope from the 16-bit field, so the shader needs one tap and no neighbours. `update()` registers the untouched texture, rewrites it (`replaceTexture` + `InvalidateTexture`, the weightmap's path) only when the field changed or the camera crossed a texel while something is near.
- **`ContentManager::loadAsset("mem://HorizonTrails")`** answers with `TrailClipmap::ensureTexture`: a material asks for the path on the first draw, before the first scene tick, and the renderers cache a miss until the content epoch moves. Without this hook the log showed the miss and the texture never appeared.
- **`TrailSystem`** (`HE_Scene/TrailSystem.h`, `SceneSystems::tickWorld` right after the weather): one `TrailField` + clipmap per world in the registry context (`TrailSystem::runtime(world)`), heals with `WeatherComponent` cur* values at the scene clock, uploads. Gameplay stamps through `runtime(world).field` (the `HE::api` wrappers are step 7).
- **Landscape graph** (`AutoLandscapeMaterial.cpp`, block "Trails"): TextureSample -> decode -> `Disturbance` and `Disturbance Slope` of both calls. Safe against a missing texture: only a texel with alpha about one half counts (a white or black stand-in has 1 or 0 there). Faded out between `kTrailFadeStart` 46 m and `kTrailFadeEnd` 58 m of camera distance, because the window wraps and belongs to this place only within 64 m. Height does not read it (interface test, unchanged cone test).
- **Slot order:** the trail takes the slot the emission order gives it (Mountains/Forest/Grassland: arrays 0, 2, 3 and trail 1, mask 13; Desert: trail 0, arrays 1..3, mask 14). The tests now compute the array mask from the texture list instead of `0x7`.
- **Debug source:** `HE_DUMP_TRAILTEST=1` (EditorApplication, next to HE_DUMP_AUTOLAND) stamps boot-sized footprints (0.8 m stride, 0.4 x 0.17 m, depth 0.9) on parallel lines over the snow plateau (x 12..60) and the plain (x -100..-30) and two tyre lines, and updates the clipmap once (the dump renders without scene ticks).

**Verified (Mac, Release):** ctest 346/346; new tests: encoding, registration, a print at the texel of its world position, rim sign (G/B left or right of the centre), the 128 m wrap, rewrite only on change, a healed field clears the picture once. Witness A/B without trails vs step 3: Mountains snow plateau and Desert sand within 1/255. With `HE_DUMP_TRAILTEST=1` footprints are visible on **Metal** (`docs/landscape-3-0-procedural-snow-sand-shots/trail-snow-footprints.jpg`, `trail-sand-footprints.jpg`: dark, rimmed prints in the snow, ripples stamped flat and shaded in the sand) and on **OpenGL** (Mountains). `missing-logs=0`.

**Not verified / open:**
- D3D11, D3D12, Vulkan at runtime: they inherit the fourth graph-texture slot (no code of their own), but the shader linking (a plain 2D sampler beside arrays, SM 5.0 sampler registers, the Vulkan 17-sampler stage) was not run. **Windows step, stays open.**
- GL logs one `UNSUPPORTED (log once): ... unit 2 GLD_TEXTURE_INDEX_2D is unloadable` warning on the first frame; it appears without trails too (the render is right), cause not traced (a unit read before its texture bound).
- Packed/exported builds: the baked material turns paths into UUIDs and `mem://HorizonTrails` has none; a shipped game must call `TrailSystem::update` (it registers the texture) before the first landscape draw, and the pak packer's treatment of the unresolvable path was not checked.
- Cost not measured (one extra tap per landscape pixel plus a 1 MB upload on a frame the field or the camera-near field changed). The camera walking over trodden ground rewrites 1 MB per texel crossed.
- The prints are a test pattern: gameplay stamping (steps, vehicles, `HE::api`) is step 7, persistence step 6 (both done below).

## Step 5c (Thema 328): dragged tracks with a heap, versus a pressed print

On the human's word: a track that is dragged through snow or sand should be continuous and pile the displaced material up on both sides; a mere press should leave only a print. The field is now signed and has two verbs.

- **`TrailField`** (`Trails/TrailField.h/.cpp`): a texel is one signed value v in [-1, 1] (16 bit, +-32767): **v > 0 a pit, v < 0 a heap**. `stampEllipse` / `stampCapsule` are **press**: pits only, never a heap. New **`plowCapsule(x0,z0,x1,z1, radius, depth, bermWidth, bermHeight, surface)`** is **drag**: the capsule's groove plus a heap on both sides (`sin^2` hump, 0 at the groove edge and at the outer foot, crest in the middle). Call it every frame with the last and the new position and the track is continuous (many short segments equal one long one, tested). A pit always wins over a heap on a texel (so the next stretch of groove clears the heap at the front of the previous one, and a foot set into a heap leaves a pit); a heap never grows on a pit and the higher of two heaps stays. Healing moves both signs toward 0 at the same rate (a heap slumps like a pit fills). `sample` / `gradient` are signed (a footstep wants `max(v, 0)`).
- **`TrailClipmap`**: R = 128 - round(127 v) (128 untouched, below a pit, above a heap), G / B the slope of v, A = 128 marker. The landscape graph decodes the pit as `Disturbance` (`saturate((128/255 - R) * 255/127)`); the heap shows only through the **slope** (it lights as a ridge). No new function pin and no new parameter.
- **Bug fixed on the way:** the rim lean had the wrong sign (`bendNormal` takes the HEIGHT gradient, the height is -v, so the lean is `-0.3 x slope`). Printed pits were lit as if raised. Fixed in `ProceduralSurfaceFunctions.cpp` (`disturbanceRim`); `matfn_gen` and all four landscape materials regenerated.
- **Debug:** `HE_DUMP_TRAILTEST=1` now has footprints (press) on parallel lines, two dragged tracks at z = -8 and z = -16 (heap both sides) and a set-down tyre line (press) at z = -22; the pose looks toward -Z.
- **Verified (Mac, Release):** ctest 345/346 in one parallel run, the one failure `test_collab_controller` (a 15 s timing test) passes alone, i.e. load flake, unrelated; the trail and material tests are green (new: press never goes negative, plow groove + symmetric heaps + continuity + step-by-step equals long segment, pit over heap, heap slump and page freed, clipmap shows heaps above 128). No-trail render vs the step 3 baseline within 1/255. Metal: dark groove with a bright heap on each side next to the footprints, in snow and sand (`docs/landscape-3-0-procedural-snow-sand-shots/trail-snow-plow.jpg`, `trail-sand-plow.jpg`); OpenGL renders the same scene (not compared pixel by pixel).
- **Not done:** the heap has no real height, only a normal lean: no silhouette, no shadow cast by it (the surface is still flat geometry). A heap-specific albedo (fluffier, brighter) is not applied. Gameplay calls (`HE::api`, footstep or vehicle emitters that choose press or drag) are step 7. D3D / Vulkan as in 5b.

### Step 5d: the heap looks like loose material (small)

The human meant by "Haufen" exactly the pile along the edge of a dragged track (what 5c builds); it read too weak. Both functions got one more appended input, `Disturbance Heap` (Float 0..1, pin 11 snow / 10 sand, default 0): snow lifts toward white (x0.55 of the way) and goes matte, sand gets up to 30 % lighter and a little rougher. The landscape decodes it from R above the middle (`saturate((R - 128/255) * 255/127)`, faded like the pit). It never touches the normal, AO or Height (tested). ctest 346/346, no-trail render within 1/255 of step 3. **Effect is modest** (snow is already near white, sand gains a lighter ridge): the pile is still only light and shading on flat geometry, no silhouette. Real height (vertex displacement on the terrain chunks, which tessellate only near the camera, or a parallax step in the material) is the open way to make it stand up; not started.

### Step 5e: the trails as real geometry (tessellation)

Wish: besides the normal-map look, the groove and the heap should have real depth and height, which needs the landscape to be fine enough where it is trodden.

- **How:** the terrain already has CPU tessellation (`TerrainComponent::tessellationFactor` 2 or 4, `tessellationDistance`): chunks near the camera get a refined mesh (`lod0Cells x factor` cells per side, Catmull-Rom height, normals, stitching to coarser neighbours), an ordinary mesh that renders the same on **all five backends** (the GPU displacement pin of Thema 207 is GL-only today, "not shown on Metal"). `generateTerrainChunkMeshTessellated` got an optional `TerrainHeightOffset` (a callback in terrain UV, `|offset| <= maxAbs`) that is added to the height, tilts the normals by its gradient (like the displacement map) and deepens the skirt. `TerrainSystem::updateTessellation` builds it from the `TrailField`: **pit lowers, heap raises, by `TrailSettings::displacementMeters` (0.30 m) x v.** The normal-map look of 5b..5d stays on top.
- **Rebuilds are bounded:** `TrailField::regionVersion(rect)` (per-page version stamps, freed pages keep theirs, so a healed pit is taken out of the mesh) says when a chunk's ground changed; `TrailField::window()` copies the chunk's rectangle once so a 66 000-vertex build does not hash-look-up per sample. A refined chunk is rebuilt at most every 10 scene ticks, from the shared 2-builds-per-tick budget, and a sculpt rebuild marks it "built without trails" (`TerrainChunkComponent::trailVersion = ~0`). A chunk with no trail under it is built exactly as before (no offset passed).
- **Resolution (the landscape needs it):** the geometry is as fine as the refined grid, cell size / factor. The witness landscape is 128 m at 129 samples (1 m) x factor 4 = **0.25 m = the field's texel**; a coarser landscape (2 m cells) gives 0.5 m geometry (a 0.5 m heap shrinks to a swell), a landscape with `tessellationFactor` 1 has **no trail geometry** (only the normal look). Set Tessellation Factor 4 and a Distance (the witness uses 80 m) on the Landscape. Visual only: collision, navigation and foliage keep the height field, so a groove is not walkable depth.
- **Debug:** `HE_DUMP_TRAILTEST=1` now also sets factor 4 / distance 80 on the witness landscape and runs the refinement ticks before the capture.
- **Verified (Mac, Release):** ctest 346/346; new tests: region versions (also after a page healed away), the window reads like the field, the refined mesh with an offset (pit floor -0.3, ridge +0.2, untouched ground unchanged, the pit wall's normal leans toward the floor, an inert offset is bit-identical to none). Captures on Metal (snow along the tracks: real footprints, a groove with walls and heaps; sand across the tracks: the two heaps as steps) and OpenGL (Mountains, same scene): `docs/landscape-3-0-procedural-snow-sand-shots/trail-snow-geometry.jpg`, `trail-sand-geometry.jpg`.
- **Not verified:** cost (a 257 x 257 chunk build is a few ms; the dump builds each chunk once, a live drag rebuilds the chunk under the camera at about 6 Hz and was not run); the live editor (inspector, play mode); a landscape that is moved or scaled (the rectangle assumes the terrain entity's position only, no rotation or scale); D3D / Vulkan (ordinary meshes, nothing backend-specific). The heap height is `bermHeight x 0.30 m` (the test pattern's 0.55 gives 0.165 m): a plough with a taller pile needs a larger `displacementMeters` or `bermHeight`.

### Step 5f: what trails cost (measured), and what was cut

Metal, Release, M5 on AC power, the witness landscape (128 m, 129 samples, refined x4 within 80 m), 1280 x 720. Scripts: `HE_DUMP_TRAILDRAG=<frames>` (+ `HE_DUMP_TRAILDRAGSPEED`, `HE_DUMP_TRAILDRAGOFF`, `HE_DUMP_TRAILTESS`) in `EditorApplication.cpp` drags a plough along the plateau one segment per frame and times, per frame, `TrailSystem::update`, `updateTessellation`, LOD, seams and `Render()`; `HE_DUMP_GPUTIME` gives whole-frame GPU time. `Render()` waits for the display (the frame is 8.3 ms at 120 Hz), so its time is not a cost; the CPU phases and the GPU time are. Medians of 5 interleaved runs; "mean / p95 / max" per frame, ms.

| | Trails (clipmap) | Refine (chunk rebuilds) | Seams | Refined chunk builds |
|---|---|---|---|---|
| Factor 1, nothing dragged | 0.003 | 0.002 | 0.003 | 0 |
| Factor 4, nothing dragged | 0.003 / 0.005 / 0.01 | 0.008 / 0.01 / 0.02 | 0.005 | 4 (the initial ones) |
| Factor 4, **dragging at 15 m/s** (0.25 m/frame, 200 frames) | 0.18 / 0.31 / ~5 | 0.70 / 7.6 / 9.5 | 0.03 | 23 |
| Factor 4, dragging at walking speed (300 frames) | 0.18 / 0.35 / ~7 | 0.74 / 7.9 / 9.4 | 0.03 | 33 |

* **GPU** (whole frame, median of 3): factor 1 6.55 ms, factor 4 idle 6.53 ms, factor 4 with a 50 m track laid **6.89 ms** (+0.35 ms, +5 %: the refined mesh is free, the track costs the clipmap tap and the extra normals). The always-on tap in the landscape shader (a missing trail texture) was not separated out.
* **What the first measurement found and the fixes:** (1) `TrailSystem::update` rewrote and re-uploaded the whole 1 MB window every frame (0.97 ms mean): the window is now re-centred only after 6 m of camera movement (`kRecenterTexels`, valid to 64 m, faded at 58 m) and otherwise only the **changed 16 m tiles** are re-encoded (0.025 ms per frame in a micro-benchmark, 0.18 ms live); (2) change tracking was per 128 m page, so a track in one corner of a page rebuilt all four 64 m chunks every 10 ticks (80 builds in 200 frames): it is per **16 m tile** now (`TrailField::regionVersion`, `changedTiles`, freed pages keep their tile versions), and a chunk that a shared tile only touches (no trail on it, none before) is skipped (`TerrainChunkComponent::trailHas`): 80 -> 23 builds; (3) every rebuild re-evaluated the whole height field (`computeTerrainHeightField`): it uses the kept `builtField` now (matters on a 1025 x 1025 landscape); (4) the height offset was evaluated five times per vertex (height and four neighbours for the normal): once per grid point now (0.4 ms of the 2 ms build).
* **What is left: the rebuild spike.** A refined chunk (257 x 257 vertices) rebuilds at most every 10 ticks while a track grows under it: one build = 1.7-1.9 ms in a micro-benchmark (`he_tests --no-skip --test-case="*cost probe*"`), but **7-8 ms** in the headless editor run, in a single frame (the p95 / max above). The same micro-benchmark under background QoS (`taskpolicy -b`, E-cores) takes 6.5-11 ms, so the gap is probably the scheduler and not the code; I could not confirm it (the headless editor under `taskpolicy -B`, `caffeinate -i` and a visible window all gave the same 8 ms). At 60 Hz (16.7 ms) a frame with a 8 ms spike still fits; at 120 Hz (8.3 ms) it would drop one frame in about six while dragging.
* **Not measured:** a foreground, windowed editor session (frame pacing felt by a person); the cost of the always-on landscape tap alone; D3D / Vulkan; a 1025 x 1025 landscape (the saving of fix 3 is real there, not timed); memory (a refined chunk is ~4 MB of CPU mesh, at most 16 per landscape; the clipmap texture 1 MB).
* **Next, if the spike matters:** keep the trail-free refined vertices per chunk and update only the vertices (and normals) inside the changed tiles instead of rebuilding the chunk (sub-millisecond), or build the mesh on the job system; a lower factor (2: refine p95 3.6 ms, 0.5 m geometry) trades resolution for it.

### Step 5g: the rebuild spike is gone (in-place vertex updates) and a real stale-texture bug

Wish: remove the spike completely. A refined chunk no longer rebuilds while a track grows under it.

- **`TerrainTessCache`** (`TerrainMeshGenerator.h`): the refined chunk is built through `generateTerrainChunkMeshTessellatedCached`, which keeps per grid point what the trail offset does not touch (height before it, the slope the normal is made from). `applyTerrainHeightOffset(mesh, cache, offset, i0..i1, j0..j1)` re-evaluates only a rectangle of vertices (and the skirt vertices under ring vertices in it): height = baseY + offset, normal from the cached slope tilted by the offset's gradient. Over the whole grid it reproduces the built mesh **bit for bit** (test), and a partial update equals a fresh build (test).
* **`TerrainSystem::updateTessellation`**: a chunk with a cache is updated vertex by vertex every tick it changed (`refreshTrailVertices`): the changed 16 m tiles (`TrailField::changedTiles(since)`) plus 2 vertices, the offset copied once for that rectangle (`TrailField::window`), the mesh edited in place (`ContentManager::getStaticMeshMutable`), `InvalidateMesh`. The seam copy (`stitchSeams`) is updated in place too, interior only; it is copied again only when the border ring moved (`applyTerrainHeightOffset` returns that). A full build (with a cache) happens only for the first refinement, after a sculpt or when no cache exists. The old 10-tick throttle only guards that full path.
* **The clipmap texture is edited in place** (`ContentManager::getTextureMutable`): no 1 MB copy and no new asset per frame.
* **Bug found by the equivalence test (it would have shown in every live session): the trail texture never updated on the GPU after its first upload.** The landscape material names it by PATH; every renderer caches a graph texture under its id when the material carries one and under its PATH otherwise, and `IRenderer::InvalidateTexture` only drops id-keyed entries (Metal: `m_graphTexCache` keyed "hi:lo"). All my earlier captures stamped before the first frame, so they looked right. Fix: the texture has a fixed id (`kTrailTextureId` = 0x41F, the free slot of the engine block) and `ContentManager` writes it into `graphTextureIds` of every material that lists `kTrailTexturePath` (`bindRuntimeTextureIds`, at load, regenerate and instance sync), so all five backends bind it by id and invalidation works. Not verified on D3D / Vulkan (they take `graphTextureIds` the same way).
* **Equality:** `HE_DUMP_TRAILDRAGPRE=1` stamps the whole track before the first frame (everything built once). A dragged-in-place result and the one-shot result have identical fingerprints of the clipmap and of every refined mesh and seam copy, and **identical pictures on Metal and OpenGL** (md5).
* **Cost, Metal, median of 5 runs, drag at 15 m/s and at walking speed (mean / p95 / max per frame):** trails 0.02 / 0.06 / 0.09 ms, refine 0.10 / 0.16 / 0.3 ms, seams 0.001 ms: **sum of the means about 0.12 ms per frame; worst frame 0.3 ms** (one run in ten shows a 3 ms outlier that is the scheduler, not the code). Before: refine 0.7 / 7.6 / 9.5 ms and trails 0.18 / 0.3 / 7 ms, i.e. an 8 ms spike every few frames. A full chunk build (first refinement, after a sculpt) is still 2-3 ms.
* **Memory:** per refined chunk the cache is 5 x N x N floats (1.3 MB at N = 257) on top of its mesh; at most 16 chunks per landscape.

## Step 6 (Thema 328): trails persist (save and load)

Trails are runtime state of a play session, not authored content: they go into the **savegame**, never into the `.hescene` (a walk over snow in the editor must not change the scene file).

- **`TrailField::serialize` / `deserialize`** (`Trails/TrailField.h`): the field as bytes ("HTR1": clock, then per page its key, surface kinds and the 16-bit values, run-length coded: a footpath is a few KB, 160 m of dragged track under 200 KB, 512 KB dense only for a page walked flat), deterministic (a loaded field saves the same bytes). `deserialize` is a validating parser (a save file is untrusted): wrong magic, truncation at any length, an absurd page count, a run past the page and a duplicated page are refused and **leave the field untouched** (tested). It loads into the live field, so the version counters go on and pages that were there but are not in the save count as freed: the clipmap and the refined chunks see everything change (`clear()` does the same).
- **`TrailField::catchUp(seconds, weather)`**: the healing of the time the field did not tick (a save loaded a day later) in one go, the same rates as `tick`, the CURRENT weather, capped at one game day, empty pages freed. An approximation (the weather in between is unknown), as the plan's 3.4 says.
- **`TrailSystem::toJson` / `fromJson`**: `{"v":1,"clock":..,"data":"<base64>"}`; clean ground is a document with empty data (loading it clears). Refuses anything that is not one of ours.
- **Savegame hook:** `HE::api::trail::save(Ctx&)` writes the document into the ACTIVE save's entity-state slot `"__trails__"`, `trail::load(Ctx&, elapsedSeconds)` reads it back and applies the healing (a save without the slot clears the trails). The game calls `trail.save` before `save.write` and `trail.load` after `save.load`; nothing is saved behind its back.
- **Not done:** streaming with the tiles of Thema 326 (that topic's tile record does not exist yet): a tile record only has to carry `TrailField::serialize()` of the pages in its rectangle (the page key is world-aligned and independent of any tile); the optional `TRAL` tail chunk of the plan is that. **Not verified:** the round trip through a real active save and `save.write` (the slot functions are tested without an active save and the JSON through `TrailSystem`; a template-backed save was not set up).

## Step 7 (Thema 328): gameplay API

`HE::api::trail` (EngineApi.h) and the registry group `trail` (so HorizonCode nodes and Lua / Python `horizon.trail` have it): `footprint(x, z, length, width, yaw, depth, surface)` and `press(x0, z0, x1, z1, radius, depth, surface)` leave **a pit only**; `drag(x0, z0, x1, z1, radius, depth, bermWidth, bermHeight, surface)` leaves a **groove with a heap on both sides** (call it every frame with the last and the new position); `depthAt(x, z)` 0..1 (what footstep audio reads, 0 on a heap), `heightAt(x, z)` metres (negative in a pit, positive on a heap), `clear`, `save`, `load(elapsedSeconds)`, `pageCount`. `surface` 0 snow, 1 sand. Without a world they do nothing and answer 0. Display names, node docs and the script group are registered; `test_hc_node_docs` and the whole suite are green. A footprint narrower than about 0.4 m hardly registers (the field's texel is 0.25 m). Not done: an engine-side emitter that stamps for characters and vehicles by itself (the movement code would call `footprint` / `drag` from its foot and wheel events): that is gameplay wiring per game, the API is what it needs. Not verified: Lua / Python / HorizonCode calls (only the registry rows and the C++ functions are tested); the C++ GameLogic service table (`HorizonGameServices.h`, a fourth place for groups that belong to GameLogic) was not extended.

## Step 4 withdrawn (2026-10-29)

The Texture Mix (commit e66ff0e3) was reverted on the human's word: snow and sand get **no texture at all**, purely procedural, as before step 4 ("es soll garkeine textur, vorher war viel besser"). The code, the four engine materials and the tests are byte-identical to step 3 (6233f51c; `git diff 6233f51c HEAD -- src tests EditorDeps` is empty), so the Desert has its `Snow Color` slot back (15 / 16 params, `autoLandscapeParamCount`) and the Snow / Grass slices are not read in the procedural roles. Do not re-add a texture overlay for the procedural roles; a "texture + procedural" idea has to come from the human again.

## Handoff

- **Topic / steps:** 328 complete on the branch `claude/schnee-sand-weiter-parameter-screenshots` (built on the 329 branch; Hive steps 1574..1581): 3 (parameters), 4 withdrawn on the human's word (no texture, purely procedural), 5 (trail field, look, clipmap texture, geometry, cost), 6 (save and load), 7 (API), 8 (this).
- **Done:** snow and sand parameters (Snow Color, Snow Look, Sand Look; 15 / 16 of 16 HeParams slots per group); `TrailField` (signed pits and heaps, press vs drag, healing, persistence); `TrailClipmap` (a 128 m wrapping texture bound by a fixed id, in-place tile updates); `TrailSystem` (scene tick, JSON); landscape wiring (pit, heap, slope into both functions); real geometry through the refined terrain chunks with in-place vertex updates; `HE::api::trail`.
- **Files that matter:** `src/HE_Core/{include,src}/Trails/` (field, clipmap), `src/HE_Scene/{include,src}/TrailSystem`, `src/HE_Scene/src/TerrainSystem.cpp` (`refreshTrailVertices`, `g_tessCaches`), `TerrainMeshGenerator` (`TerrainTessCache`, `applyTerrainHeightOffset`), `ProceduralSurfaceFunctions.cpp`, `AutoLandscapeMaterial.cpp` (block "Trails"), `ContentManager.cpp` (`bindRuntimeTextureIds`, the `loadAsset` hook), `EngineApi.{h,cpp}` (namespace `trail`), `tests/test_trail_field.cpp`, the witness switches `HE_DUMP_TRAILTEST` / `HE_DUMP_TRAILDRAG*` in `EditorApplication.cpp`.
- **Verified (Mac, Release, Metal and OpenGL):** ctest 346 / 346; no-trail render within 1/255 of the step 3 baseline; a dragged-in-place track equals the one-shot build bit for bit (clipmap, meshes, seam copies, pictures on both backends); drag cost about 0.12 ms per frame, worst frame 0.3 ms; GPU +0.35 ms for a laid 50 m track.
- **Not verified:** **D3D11, D3D12, Vulkan at runtime** (the fourth graph-texture slot, shader link and sampler limits, `InvalidateTexture` by id; the tessellated meshes are ordinary meshes) **stays an open Windows step**; a windowed foreground editor session and the Landscape inspector (set Tessellation Factor 4 and a Distance for trail geometry; 1 m cells x 4 = 0.25 m); a landscape that is rotated or scaled (the chunk rectangle assumes position only); a packed build (`mem://HorizonTrails` / id 0x41F is a runtime texture: a shipped game must call `TrailSystem::update` before the first landscape draw, and the pak packer was not checked); the save round trip through a real active save; Lua / Python / HorizonCode calls of the new rows; memory under a 16-chunk worst case.
- **Decisions taken:** CPU field as the authority and a CPU-filled clipmap texture instead of the plan's GPU compute / FBO path; 16-bit pages; `eff = D` for snow; press versus drag as two verbs; trail geometry through the existing CPU tessellation (all five backends) rather than the GL-only GPU displacement pin; the texture bound by a fixed id so it can be refreshed; saved in the savegame, not the scene.
- **Decisions suggested, not taken:** an engine-side foot / wheel emitter; carrying the field in the tile record of Thema 326; raising the HeParams budget (16, in every renderer) if a later feature needs a slot (Forest and Grassland have one free, the Desert none).
- **Traps:** a function default or body change means `matfn_gen` AND `landscape_tex_gen --material` for all four groups, and the editor reads function assets from its deploy's `EngineContent/MaterialFunctions` too; the landscape material has four graph textures (three arrays + the trail), the array mask is computed from the texture list (13 / 14), not 7; a texture that changes at runtime must be bound by id (5g); the headless dump renders without scene ticks (the witness calls `TrailSystem::update` itself); under `taskpolicy -b` the same code is 5 times slower (timings from a hidden-window run are pessimistic); `test_collab_controller` is a timing test that fails now and then under parallel load.
- **Next steps, in order:** 1. Windows: run the landscape witness (`HE_DUMP_AUTOLAND=1 HE_DUMP_TRAILTEST=1`) on D3D11 / D3D12 / Vulkan; 2. open the PR for the branch (steps 3 to 7, base: the 329 branch); 3. gameplay emitters (footsteps, vehicles) in the movement / vehicle code with `trail::footprint` / `drag`; 4. the `TRAL` chunk in 326's tile record when that exists.
