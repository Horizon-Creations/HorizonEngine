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

## Step 4 (Thema 328): texture + procedural

- **What:** new parameter `Texture Mix` (Vec4, x = snow, y = sand, z/w unused, default 0). x mixes the group's **Snow texture** into the procedural snow, y mixes the **ground (Grass slot) texture** into the procedural sand (Desert only). `AutoLandscapeMaterial.cpp`, block "Texture + procedural" right after the gate helpers: plain (unbombed) read of the slice, behind an `If` on `mix > 0`, then the same `blend` (Lerp over albedo, normal, mask) as the layers, `saturate(mix)` as weight; the mixed height replaces `snow.height` / `grass.height`, so every later bias sees it.
- **Cost at the default:** no tap (the If is a uniform branch), result = procedural surface (`mix(a, b, 0) = a`).
- **Slot budget:** every group is now at 16. The Desert's `Snow Color` slot went to the Texture Mix (its snow colour is a baked constant, like its sand colour); the other groups have no `Sand Look`. `autoLandscapeParamCount` is gone, `kAutoLandscapeParamCount` = 16 for all.
- **Verified (Mac, Release, Metal):** ctest 345/345 (two assertions changed on purpose: the Snow slice and the Desert's Grass slice are now read, exactly 3 plain reads each, never bombed; reads 18 -> 21). Witness A/B at the default: Mountains top bit-identical, Mountains snow plateau max 1/255, Desert sand close: one pixel of 921 600 differs (367,328, a glitter sparkle, 101/255), everything else within 1/255. Effect: a temporary build with the default 0.7 (deployed material swapped, restored, assets regenerate byte-stable) shows the real snow texture in the snow (mean 6.2/255 from the default) and the placeholder ground array in the sand (mean 24/255): `docs/landscape-3-0-procedural-snow-sand-shots/snow-texture-mix-0.7.jpg`, `sand-texture-mix-0.7-placeholder-array.jpg`.
- **Not verified:** the Vec4 widget in the instance panel; the real Desert ground texture (only the 128 px placeholder array is on this Mac); OpenGL / D3D / Vulkan (the new reads are the layers' own plain reads, no new sampler: the three arrays are the same).
- **Decisions taken:** plain read, no bombing (a mixed overlay hides tiling less than a layer; add `seed` later if it shows); per-role amount, not per-pixel; Height of the mixed result feeds the biases.
- **Suggested, not taken:** a paint-layer weight for the mix; taking the texture's height to modulate the procedural ripples instead of a plain lerp.

## Handoff (steps 3 and 4)

- **Topic / step:** 328, steps 3 (Hive 1576) and 4 (Hive 1577). Branch `claude/schnee-sand-weiter-parameter-screenshots`.
- **Decisions taken:** Vec4 packing; sand colour baked; Desert = 16 params. **Suggested, not taken:** raise the slot budget (needs all five renderers + Windows verification), group-specific defaults from the picker values.
- **Trap (budget):** every group sits at 16/16 HeParams slots; the next parameter (Path layer, trails) needs a packed slot or the renderers' budget raised. 329 step 3 (Path layer) edits the same file and also needs parameters; there are no free slots left in the Desert.
- **Next steps:** 1. live check of the Vec4 widgets in the instance panel; 2. step 5 trails (Disturbance pin; trail settings are CPU-side, no slot); 3. real Desert arrays to judge the sand mix.
