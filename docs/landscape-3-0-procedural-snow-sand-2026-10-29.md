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

## Handoff (steps 3, 5a; step 4 withdrawn)

- **Topic / step:** 328, step 3 (Hive 1576) done, step 4 withdrawn (1577), step 5 done on Metal + OpenGL (Hive 1578: CPU field, function look, clipmap texture, landscape wiring, 5c: press vs dragged track with heaps); D3D/Vulkan runtime check open. Branch `claude/schnee-sand-weiter-parameter-screenshots`.
- **Decisions taken:** Vec4 packing; sand colour baked; Desert = 16 params. **Suggested, not taken:** raise the slot budget (needs all five renderers + Windows verification), group-specific defaults from the picker values.
- **Trap:** 329 step 3 (Path layer) edits the same file and also needs parameters; there are no free slots left in the Desert.
- **Next steps:** 1. live check of the Vec4 widgets in the instance panel; 2. step 4 (mix texture + procedural, a Mix parameter will need a slot: free one or pack); 3. step 5 trails (Disturbance pin).

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

- **`HE::TrailClipmap`** (`Trails/TrailClipmap.h/.cpp`): a 128 m x 128 m window around the camera as a 512 x 512 RGBA8 texture, path `mem://HorizonTrails` (no file, so the EngineContent resolver never looks for it), `srgb = false`, one mip. World-aligned and toroidal: the texel of (x, z) is (floor(x/0.25) mod 512, floor(z/0.25) mod 512), the shader uv is `xz / 128` with the sampler's repeat wrap, so **no window origin travels to the GPU** (Desert is at 16/16 parameters). R = 255 - depth8 (white = untouched), G / B = slope x / z (128 = flat, +-8 per metre), A = 128 marker. The CPU computes the slope from the 16-bit field, so the shader needs one tap and no neighbours. `update()` registers the untouched texture, rewrites it (`replaceTexture` + `InvalidateTexture`, the weightmap's path) only when the field changed or the camera crossed a texel while something is near.
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
- The prints are a test pattern: gameplay stamping (steps, vehicles, `HE::api`) is step 7, persistence step 6.

## Step 5c (Thema 328): dragged tracks with a heap, versus a pressed print

On the human's word: a track that is dragged through snow or sand should be continuous and pile the displaced material up on both sides; a mere press should leave only a print. The field is now signed and has two verbs.

- **`TrailField`** (`Trails/TrailField.h/.cpp`): a texel is one signed value v in [-1, 1] (16 bit, +-32767): **v > 0 a pit, v < 0 a heap**. `stampEllipse` / `stampCapsule` are **press**: pits only, never a heap. New **`plowCapsule(x0,z0,x1,z1, radius, depth, bermWidth, bermHeight, surface)`** is **drag**: the capsule's groove plus a heap on both sides (`sin^2` hump, 0 at the groove edge and at the outer foot, crest in the middle). Call it every frame with the last and the new position and the track is continuous (many short segments equal one long one, tested). A pit always wins over a heap on a texel (so the next stretch of groove clears the heap at the front of the previous one, and a foot set into a heap leaves a pit); a heap never grows on a pit and the higher of two heaps stays. Healing moves both signs toward 0 at the same rate (a heap slumps like a pit fills). `sample` / `gradient` are signed (a footstep wants `max(v, 0)`).
- **`TrailClipmap`**: R = 128 - round(127 v) (128 untouched, below a pit, above a heap), G / B the slope of v, A = 128 marker. The landscape graph decodes the pit as `Disturbance` (`saturate((128/255 - R) * 255/127)`); the heap shows only through the **slope** (it lights as a ridge). No new function pin and no new parameter.
- **Bug fixed on the way:** the rim lean had the wrong sign (`bendNormal` takes the HEIGHT gradient, the height is -v, so the lean is `-0.3 x slope`). Printed pits were lit as if raised. Fixed in `ProceduralSurfaceFunctions.cpp` (`disturbanceRim`); `matfn_gen` and all four landscape materials regenerated.
- **Debug:** `HE_DUMP_TRAILTEST=1` now has footprints (press) on parallel lines, two dragged tracks at z = -8 and z = -16 (heap both sides) and a set-down tyre line (press) at z = -22; the pose looks toward -Z.
- **Verified (Mac, Release):** ctest 345/346 in one parallel run, the one failure `test_collab_controller` (a 15 s timing test) passes alone, i.e. load flake, unrelated; the trail and material tests are green (new: press never goes negative, plow groove + symmetric heaps + continuity + step-by-step equals long segment, pit over heap, heap slump and page freed, clipmap shows heaps above 128). No-trail render vs the step 3 baseline within 1/255. Metal: dark groove with a bright heap on each side next to the footprints, in snow and sand (`docs/landscape-3-0-procedural-snow-sand-shots/trail-snow-plow.jpg`, `trail-sand-plow.jpg`); OpenGL renders the same scene (not compared pixel by pixel).
- **Not done:** the heap has no real height, only a normal lean: no silhouette, no shadow cast by it (the surface is still flat geometry). A heap-specific albedo (fluffier, brighter) is not applied. Gameplay calls (`HE::api`, footstep or vehicle emitters that choose press or drag) are step 7. D3D / Vulkan as in 5b.

## Step 4 withdrawn (2026-10-29)

The Texture Mix (commit e66ff0e3) was reverted on the human's word: snow and sand get **no texture at all**, purely procedural, as before step 4 ("es soll garkeine textur, vorher war viel besser"). The code, the four engine materials and the tests are byte-identical to step 3 (6233f51c; `git diff 6233f51c HEAD -- src tests EditorDeps` is empty), so the Desert has its `Snow Color` slot back (15 / 16 params, `autoLandscapeParamCount`) and the Snow / Grass slices are not read in the procedural roles. Do not re-add a texture overlay for the procedural roles; a "texture + procedural" idea has to come from the human again.
