# Tile landscape: what Windows still has to take in (Thema 326, 2026-11-21)

Everything of 21.11. was built and verified on a Mac (Apple M5: Metal, OpenGL 4.1, Vulkan through MoltenVK). Nothing ran on Windows.
This is the list for NN-WS03 (RTX 4070: D3D11, D3D12, Vulkan, OpenGL 4.6), in the order that makes sense. Branch
`claude/landscape-weltmassstab-100km`, PR 214. The three docs of the day hold the detail:
`landscape-world-scale-far-image-`, `-writeback-` and `-render-window-2026-11-21.md`.

## 0. First: build and run what is there

```
cmake --build <build> --target he_tests landscape_gpu_check HorizonEditor HorizonGame
he_tests --source-file=*test_landscape*            # 192 cases on the Mac
landscape_gpu_check --only gl | d3d11 | d3d12 | vulkan
```

Expected on Windows **as the code is now**: scenes 1 to 3 as before; scene 4 "far image" prints `this backend does not take the far
image yet` on D3D11, D3D12 and Vulkan and passes (a marker image must change nothing), and passes with real numbers on OpenGL; scene 5
"edit": the near dab moves pixels everywhere, the far stroke moves pixels on OpenGL and Vulkan and 0 px on D3D (said, not failed);
scene 6 "render window" must pass on **every** backend (0 pixels apart on the Mac), and on D3D it prints that the atlas began again
(item 2a). On OpenGL 4.6 the
reflection check runs for real (it is `NOT CHECKED` on a Mac).

## 1. Windows CI is red: find out why `test_landscape_physics.cpp:298` fails

Run 44931549477 (commit `71b3f3f00`): Linux, Linux Vulkan and macOS green, Windows red in three files.
`test_physics.cpp` (2488, 3129, 3145) and `test_animation_ik.cpp` (705, 706) fail with the same lines on the head of `release/0.8.0`
(`18c493b0d`, run 44741828354): they came with the merge (step 25) and are not this topic's.
`test_landscape_physics.cpp:298` (`CHECK(y > groundY - 0.5f)`, a crate dropped on a tile's ground body goes through) exists only on
this branch, and the branch's last green run is from before the merge. **Not proven** to be the same regression. Check it first: if the
release's physics failures are fixed and this one stays, it is ours.

## 2. Vulkan: the far image and the coarse revision (board step 27)

* `LandscapeRenderStatic::farW / farH / farMips` (sRGB RGBA8 with its mip chain) is uploaded by `OpenGLLandscape.cpp:237` and
  `MetalLandscape.mm` only. `VulkanLandscape.cpp:481` binds a stand-in at `kLandscapeVkBindingFar` (13,
  `LandscapeShaderSource.h:258`) and `uTexMaps.z` stays 0. To do: upload (`R8G8B8A8_SRGB`, mips, a linear + clamp sampler), bind,
  set `g.texMaps[2]` next to `VulkanLandscapeLayout.h:92`. The GLSL is shared and already has the branch (`LandscapeShaderSource.h:170`).
* `LandscapeRenderStatic::coarseRevision` is **done on Vulkan** (21.11., `VulkanLandscape.cpp`, run through MoltenVK on the Mac: far
  stroke 57 px, window move 30 of 191 tiles). On the RTX 4070 it only has to be run. What it means is in item 2a.
* Then: `VulkanBackend::takesFarImage()` returns true (`GpuCheck.h:63`), Vulkan joins the `takes` list in `EngineCheck.cpp:150`.
  Acceptance: `landscape_gpu_check --only vulkan` exit 0 with marker pixels in scene 4 and in the engine pass "far image", and the
  far stroke of scene 5 moving more than 30 px.

## 2a. What `coarseRevision` means now, and why D3D has to take it (the smooth window move)

The owner's rule: **a render window that moves must never throw the near ground away.** OpenGL, Metal and Vulkan follow it since
21.11. (Vulkan's coarse refresh is in, only its far image upload is still step 27). D3D11 and D3D12 do not yet, and until they do they
get the old behaviour through a switch, so nothing is broken there:

* `IRenderer::Capabilities::landscapeCoarseRevision` (`IRenderer.h`). True on Metal, OpenGL, Vulkan. **False on D3D11/D3D12.**
* The application hands it to the runtime: `m_landscape.setWindowKeepsAtlas(renderer()->GetCapabilities().landscapeCoarseRevision)`
  (`EditorApplication.cpp`, `GameApplication.cpp`, next to `SetLandscapeProvider`).
* With it **false**, a window move is announced as a new landscape (`LandscapeRuntime::placeWindow`): new `generation`, the atlas
  begins again, about 25 frames of far levels near the camera. Correct, but the pop the owner does not accept.
* With it **true**, a window move changes `LandscapeRenderStatic::coarseRevision` only, and the frame's static is another object with
  another `tilesX / tilesZ / worldMax*`, other coarse images **possibly of another size** (at the world's end the window is smaller)
  and another far image. The backend has to, when `generation` is the same and `coarseRevision` differs:
  1. make `coarseHeights` and the three `coarseWeights` images again **at their new sizes** (not an in-place copy), and the far image;
  2. leave the tile height and weight arrays and the slot table exactly as they are, and execute the frame's uploads and table updates
     as on any frame (they are few: only the tiles the step brought in);
  3. take every uniform from the frame's static as always (`uWorld`, `uHeightInfo`, `uCoarseGrid`: they describe the window);
  4. remember the revision; and not free an image a frame in flight still reads (D3D12: defer the release like any other resource of a
     frame in flight; D3D11 is immediate-context and may just replace them).
  The models: `OpenGLLandscape.cpp` (the `else if (m.coarseRevision != w.coarseRevision)` block), `MetalLandscape.mm` (new textures,
  the old ones live on in the frame that holds them), `VulkanLandscape.cpp` (flush, wait idle, destroy and remake the four images).
* **Then** set `c.landscapeCoarseRevision = true` in `D3D11Renderer::GetCapabilities` / `D3D12Renderer::GetCapabilities`, and return
  true from `refreshesCoarse()` in the tool's `D3D11Backend.cpp` / `D3D12Backend.cpp`. Not before: with the flag on and the block
  missing the far levels would show the previous window's ground at the new place.
* Acceptance (`landscape_gpu_check`, scene 6, per backend): `0 of 129600 pixels` apart from the world drawn whole, **and**
  `the move uploaded 30 tile(s) of the 191 near the camera` (the Mac's numbers on all three; the check fails if half or more of the
  near tiles are uploaded again). Scene 5's far stroke then moves pixels too (57 px on the Mac). `tests/test_landscape_window.cpp` has
  the runtime's half ("a window move keeps the atlas").

## 3. D3D11 / D3D12: the same two things (board step 28)

* The HLSL landscape shader (`kLandscapeHLSL`, `LandscapeShaderSource.h:568`, one source for both) has **no far branch**. Port what
  `kLandscapeFS` does for `uTexMaps.z > 0.5 && slot == 65535`: one `SampleGrad` of an sRGB RGBA8 texture with mips (rgb = albedo,
  a = AO) instead of the weights and the arrays, every derivative taken before the branch. Upload `farMips` in `D3D11Landscape.cpp` and
  `D3D12Landscape.cpp`, set `g.texMaps[2]` next to `LandscapeD3D.h:90`. `tests/test_landscape_shader.cpp` lists the uniforms per port.
* `coarseRevision` as in item 2a, including the capability flag and the tool's `refreshesCoarse()`.
* Acceptance as in items 2 and 2a, with `--only d3d11` and `--only d3d12`. Real hardware; do not mark done from CI alone.

## 4. Already in, to be confirmed by a run

* `uvOrigin` (`LandscapeD3D.h:86`, `VulkanLandscapeLayout.h:88`): the world-UV phase of the ground textures reads the absolute floating
  origin instead of the frame's (which is the render window's now). One line each, compiled by CI at best, never run on D3D.
* `D3D12Renderer::RenderSceneImage`: the merge moved the restore into `rvtStillLeave()`; `drainGpuDebugMessages()` followed it there.
* Scene 6 on D3D: a window move is a new generation, the path a new landscape has always taken there.

## 5. By hand in the editor (any machine, never done)

1. A scene with a Tile Landscape, Landscape mode, section "Tile Landscape", **Edit Tiles** on: the ring follows the ground, a stroke
   raises it at once, Undo Stroke takes the whole stroke back, Save Tile Edits writes `<pack>.edits` beside the pack, closing the editor
   with unsaved strokes asks.
2. Export with "Bake far landscape" on: the cook step's log has `Landscape far image: N region(s) ...: B baked, U unchanged`; a second
   export says `0 baked`; with an edits file also `Landscape edits: ... is baked into the shipped pack`. Start the exported game.

## 6. Open design work, not Windows-specific

* **A window move still pops on D3D11 / D3D12** (worlds over 131 km only) until item 2a is done there. On OpenGL, Metal and Vulkan it
  is smooth since 21.11.: the atlas stays, only the tiles the step brings in are uploaded.
* The landscape **material** in the tile world (Thema 335 step 10, needs PR 233): `kLandscapeFS` is a fixed layer blend, not the Auto
  Landscape graph. When it draws the real material, `landscapeFarWeightPainter` is replaced by a painter that photographs it.
* One Landscape mode for both kinds of landscape (today the tile brush is its own small tool in `LandscapeWorldTools.cpp:500`).

## 7. Everything that is still missing after 21.11. (owner asked for the full list)

Done on the Mac the same evening and only to be **run** on Windows: streaming switched off (`LandscapeWorldComponent::streaming`,
"Stream Tiles", worlds up to 1024 tiles), a migrated terrain bringing its water, the textured far-image witness (scene 4 prints
`textured ground: ... mean gap 0.62/255` on OpenGL and Metal; D3D and Vulkan print it once they take the far image).

Not built anywhere, largest first. None of it is Windows-specific, but each part that draws needs its D3D and Vulkan half on NN-WS03:

| # | Missing | Why it was not done in the session | Where it starts |
|---|---|---|---|
| 1 | **The landscape material in the tile world** (Auto Landscape graph, RVT, snow / sand / weather / trails) | The tile landscape does not go through the engine's material and lighting pipeline at all: `kLandscapeFS` is its own shader with its own sun and ambient, no shadows, no fog, no GI on it. Drawing the material means a landscape variant of every material shader (vertex stage from the height atlas, weights from the tile arrays instead of `heLandscapeWeights`) inside each backend's scene pass. That is the renderer step of weeks, per backend, not a hook | `LandscapeShaderSource.h`, `MatShaderGen`, each backend's `*Landscape.*` and scene pass; RVT 2.0 is on the branch since `11b9eea4e` (Thema 335 step 10) |
| 2 | The far image painted from that material | Waits for 1: a far image of the real material beside near ground that is the layer blend is a visible ring. The painter is the one swappable piece (`LandscapeFarPainter`); `HlodBaker::bakeRect` photographs any world rectangle of the old material already | `LandscapeFarBake.h`, `LandscapeHlodBaker.h` |
| 3 | **Shadows, fog and GI on the tile landscape** as on any other object; GI sun shadows on Metal and D3D12; reflections on four backends | Part of 1 (lighting pipeline) for the first half; the second needs the height field in Metal's and D3D12's ray-traced paths | `docs/landscape-world-scale-gi-2026-11-02.md`, `docs/landscape-reflections-design-2026-11-19.md` |
| 4 | **Tools on tiles**: foliage painting, water brush and lakes, Mountain, Ramp, heightmap import onto an existing world | Each writes another raster back (foliage masks are not in the tile format at all; water rasters are, editing them is not built). Sculpt and Paint Layer exist | `LandscapeEdit.h` (the write-back to copy), `TerrainTools.cpp` (the brushes), `LandscapeTile.h` (format) |
| 5 | **One Landscape mode** for both kinds, strokes in the editor's own undo | 2871 lines built around an entity's terrain; needs a target abstraction. Today: "Edit Tiles" in the Tile Landscape section, its own Undo Stroke | `TerrainTools.cpp`, `LandscapeWorldTools.cpp:500` |
| 6 | **Migration when a scene is loaded**, with foliage masks and material overrides | The migration is a button ("Migrate Terrain to Tiles"); it brings heights, paint and now water. Masks and overrides have nowhere to go until 1 and 4 | `LandscapePackBuild.cpp` (`migrateTerrainToPack`), `SceneSerializer.cpp` |
| 7 | **Colour on the cell proxies** (far buildings and props are silhouettes in the default material) | A mesh has no vertex colours (position, normal, uv). Planned: one small palette texture for the whole table, each instance's uv pointing at its mean albedo, one shared material; needs a version of the `'HLDF'` table | `LandscapeCellHlodBake.cpp`, `LandscapeHlodProxies.cpp` |
| 8 | **Entity cells streaming in the editor** | `CellRuntime` is wired in `GameApplication` only | `GameApplication.cpp` (`updateCellStreaming`), `EditorApplication.cpp` |
| 9 | Script API, MCP tools, tutorial, outliner, viewport picking, spline tool for the tile world | They address a `TerrainComponent` entity | `EngineApi.cpp`, `McpToolsTerrain.cpp`, `ViewportPick.cpp`, `SplineTool.cpp` |
| 10 | The end of a brush stroke without dropping the changed tiles once | Ground bodies, foliage, navigation and water are rebuilt by the tile leaving the store for a frame or two; an in-place refresh needs a "tile changed" on each of the four | `LandscapeEditSession::endStroke`, `LandscapePhysics/Foliage/Navigation/Water` |
| 11 | Huge worlds: the tile index, the cell HLOD table and the pack writer's coarse world are still whole in memory; no pack over 131 km was ever built or flown | Not needed up to 131 km | `LandscapeSource.h` (the two `shortcut:` comments) |
| 12 | Collaboration on a pack (two people sculpting) | The collaboration layer streams scenes, not packs | `TerrainSync.*` is the model for terrains |

### 7a. Late on 21.11.: rows 1, 3 and 7 are built on Metal, and what Windows has to do for them

Read `docs/landscape-tile-material-design-2026-11-21.md` (its Handoff) first. In short:

* **Row 1 and the first half of row 3 are done on Metal.** The ground is drawn with `LandscapeRenderFrame::materialId` (the landscape
  entity's `MaterialComponent`, else its texture group's Auto Landscape material), takes cascade and ray-traced shadows, casts into
  the shadow maps, is fogged. A backend says so with `IRenderer::Capabilities::landscapeMaterial`; without it the fixed blend stays,
  which is what D3D11, D3D12, Vulkan and OpenGL do today. `landscape_gpu_check` prints `NOT CHECKED: this backend draws the tile
  landscape with its fixed blend only` for them, and must print the five witness lines once the flag is set.
* **Row 7 is done** (`028e0bf31`): palette in the `'HLDF'` table (version 2, version 1 still read), one material for all proxies.
  Backend-neutral, only to be run.
* **What a D3D / Vulkan port needs** (the Metal code is the model: `GetOrBuildLandscapeMaterialPipeline`, `EncodeLandscapePass`):
  1. The shaders are there for every backend: `MaterialShaderLibrary::landscapeVertex(backend)` and
     `landscapeFragment(hash, glsl, backend, clustered)`. They compile for SPIR-V (tested). **HLSL is not tested and will not fit as it
     is**: the fragment adds one constant buffer (binding 50) and seven textures (51-57) read with `texelFetch` only. SM 5.0 has no
     sampler register left, so they must be pinned to free `t` registers with no `s` (the pattern of the RVT cold pages, `HE_RVT_COLD_FETCH`
     in `fragmentVariant`), and 14 constant-buffer registers is the limit for `b`.
  2. Vulkan: bindings 50-60 in the material descriptor set layout (51 is shared by both stages), filled from `VulkanLandscape`'s
     images; `LsGlobals` at 50 is the head of the block `VulkanLandscape` already uploads; `LsPatch` (58) is a uniform block here, not
     the push constant of the landscape's own pipeline.
  3. The depth convention: the vertex stage multiplies by `uViewProj` as given. Metal's scene pass hands the GL matrix; the shadow and
     GI passes hand `kMetalClipFix * viewProj` and ask the provider with `zeroToOne = true`. Take each pass's own matrix.
  4. Each extra pass asks the provider again with its own view (`m_landscapeProvider(clip, zeroToOne, frame)`): the patches are that
     view's, the LOD levels stay the main camera's, and the frame carries whatever uploads were pending, so every call's uploads must
     be applied (the first pass of a frame is the one that gets them).
  5. Do not cull back faces for the ground in a shadow pass.
  6. Add the backend to the `landscapeMaterial` capability only when `landscape_gpu_check` passes there.
* **Still open everywhere** (second half of row 3): with ray-traced GI on, the ground is not in the acceleration structure, so it
  receives ray-traced shadows but casts none and bounces no light. SSAO and velocity pre-passes do not have the ground. Rows 2, 4
  (Roughen is now on the tile brush; Ramp, Mountain, foliage, water, import are not), 5, 6, 8-12 are as the table says.

Never measured on any machine: the 100 km flight of the original plan (1 % low under 8 ms, no hitch over 16 ms, memory under budget),
the cost of a far bake, of a window move, of a brush stroke. The first Windows session with the hardware free should take those.

### 7b. 22.11.: virtual texture, stamps, foliage mask and water brush on tiles, and what Windows has to do for them

Built and checked on the Mac (Metal); details in `docs/landscape-tile-material-state-2026-11-22.md`.

* **Formats changed, old files still load** (tested): a tile blob may carry one more optional stream (flag bit 2 of byte 6, the
  painted foliage density); an edits file may carry a tail after its tile records (tag `HLWT`, the water table a brush changed).
  A build from before reads both and ignores the new part. **Windows: run `he_tests --source-file=*test_landscape_edit*`**; it
  writes and reads both forms.
* **Virtual texture on the tile ground** (`RvtLandscapeTiles.cpp`): only a backend that says `Capabilities::landscapeRvt` gets
  it, today Metal. For D3D11 / D3D12 / Vulkan it needs three things, all with a Metal reference in `MetalRenderer.mm`:
  `SetRvtTileGround` + the patch split (`TakeRvtTilePatches`, patches wholly inside the rectangle drawn with the consumer),
  a page camera with `EditorCameraOverride::rvtTileGround` that draws the ground with `rvtMaterial` and no meshes of the live
  world (the driver hands an empty world), and the consumer bound for the landscape draw (`BindRvtForDraw`). Until then those
  backends compute the ground in place: correct, slower. Witness: `landscape_gpu_check`, the four `virtual texture ...` lines.
* **Landscape Stamp** (`LandscapeStampComponent`, `LandscapeStamps.h`): CPU only, nothing per backend. By hand on Windows: place
  one with the tile brush's Stamp tool, move it in the Scene mode, export, and look at the exported game (never done anywhere).
* **Foliage Mask and Water tools** of the tile brush: CPU only. By hand: paint both, save, reopen, export.

Added later on the 22nd (after the lines above were written):

* `IRenderer::SetRvtTileGround` has a fourth parameter `nativeNearM` (default 0 = off): patches nearer than that keep the
  ground's own material. A backend that implements the call takes it too (Metal: `TakeRvtTilePatches`). The setting is
  `ProjectRvtSettings::tileNativeNear`, off by default; `tests/fixtures/game_build_config/HEGameBuildConfig.h` has the new
  field, so **`test_game_build_config` must pass on Windows as it does on the Mac** (line endings are the usual suspect).
* `LandscapeRuntime::coarseChanged(keys)`: pages painted from far data go stale when it changes. Nothing per backend.
* Landscape Stamp: `heightmapFile` (16-bit PNG / PGM / `.r16` through `TerrainHeightmap::readGrey16`, which got a `channel`
  parameter) and `channel` (red, green, blue, alpha, luminance). CPU only. **By hand on Windows: the file dialog
  (`LandscapeWorldTools::browseStampFile`, SDL) and a path with backslashes or a drive letter**; relative paths are resolved
  against the project folder with `std::filesystem`, which was only run on macOS.
* What each backend has of the tile landscape today (state doc, table at the top): Metal everything; Vulkan the material in the
  scene pass only (no `landscapePasses`, no `landscapeRvt`); OpenGL the material only where the driver has 24 samplers, never
  run; D3D11 / D3D12 the fixed blend.

## 8. The tile ground on every backend: what is proven, what is written and waits for a run, what is not built (22.11.)

Written from the Mac at the owner's request ("catch up Vulkan, both D3D backends and OpenGL as far as you can, so that Windows only
tests and fills gaps"). Three states: **proven** = ran and judged by a witness on a machine; **written** = code is in, never run
on a machine, behind a switch; **not built**.

**The switches depend on the platform** (owner's decision, `LandscapeRenderFrame.h`): a written pass is ON by default on the
platform where it can run and be judged, off elsewhere, and the variable forces it either way (`=1` / `=0`).
`HE_LANDSCAPE_D3D_PASSES`: on under Windows. `HE_LANDSCAPE_VK_SSAO`: on with a real Vulkan driver (Windows, Linux), off on macOS
(MoltenVK). **So on Windows the unrun D3D shadow code and the unproven Vulkan SSAO pass are live from the first start**: if the
ground or the shadows look wrong, set the variable to `0` first to see whether it is this code.
The editor lists what the running renderer says it can do under Preferences > Display > Backend > Renderer Capabilities
(`EditorSettingsPanel.cpp`): the "Tile landscape: ..." lines are `landscapePassMask`, so a pass that is switched off reads "no".

One tool judges all of it: `landscape_gpu_check --only <gl|vulkan|d3d11|d3d12> --arrays <EngineContent> --out <dir>`. It prints
`(passes this backend says it has the ground in: shadow .., GI .., SSAO .., velocity .., G-buffer ..)`: a pass a backend claims
(`Capabilities::landscapePassMask`, bits `kLandscapePass*` in `IRenderer.h`) is a `FAIL` when its witness does not hold, one it
does not claim prints `not built on this backend`. A backend without `landscapeMaterial` (both D3D, macOS OpenGL) runs the short
form `fixedBlendPasses` (`EngineCheck.cpp`).

| | Metal | Vulkan | OpenGL | D3D11 | D3D12 |
|---|---|---|---|---|---|
| Ground drawn with its material | proven (M5) | proven (MoltenVK) | written, needs 24 fragment samplers: never run | shaders proven offline, C++ not built | as D3D11 |
| Ground casts into the shadow layers | proven | proven (MoltenVK): 4/255 against 176 | proven (macOS GL): 150 against 223 | **written**, switch | **written**, switch |
| Ground in the SSAO position pre-pass | proven | **written**, switch: witness reads 0 on MoltenVK | not built | not built | not built |
| Ground in the GI pre-pass / ray structure | proven | not built | not built | not built | not built |
| Ground in the velocity pass (TAA) | built, capture cannot show it | not built | not built | not built | not built |
| Ground in the G-buffer (deferred) | proven | not built (drawn forward on top, lit) | not built | not built | not built |
| Virtual texture for the ground (`landscapeRvt`) | proven | not built | not built | not built | not built |

**What to run on Windows, in this order**

1. `he_tests` whole, then `he_tests --source-file=*test_landscape*` and `-tc="tile landscape variant*"`. The second test case of
   that name is the D3D shader contract: every register of the generated HLSL inside Shader Model 5.0, the landscape's data at
   `MaterialShaderLibrary::kHlslLandscapeGlobalsReg` (b12, b13) and `kHlslLandscapeFirstSrv` (t47..t55).
   **FXC has never seen these shaders** (glslang's HLSL front end parsed both on the Mac, which is not the same compiler): compile
   the two texts with `D3DCompile` (`HE_DUMP_LS_HLSL=<dir>` makes the test write `landscape_vs.hlsl` / `landscape_ps.hlsl`).
2. Shadows, D3D11: `landscape_gpu_check --only d3d11 ...` (the pass is on under Windows). Must print
   `the ground casts (fixed blend): a box under the ground is A/255, and B/255 when the ground is not in the shadow maps` with
   A below 0.8 x B, and `OK (fixed blend: the shadow maps)`. With `set HE_LANDSCAPE_D3D_PASSES=0` the same line shows A = B and
   `not built on this backend`. Code: `D3D11Landscape::drawDepth`, `renderDepthLayer` in `D3D11Renderer.cpp` (search
   `landscapeD3dPassesOn`). Things I could not check: that the shadow pass's depth-stencil state is LESS with writes when the
   ground is drawn (it draws with whatever is set), and that `StateGuard` puts the depth vertex shader back for the next layer.
3. Shadows, D3D12: the same with `--only d3d12`. Code: `D3D12Landscape::drawDepth` + `depthPsoGet`, `renderDepthLayer` in
   `D3D12Renderer.cpp`. Unchecked: the depth-only PSO against the shadow array's format (assumed `D32_FLOAT`, as `depthPSO`),
   the root state put back after the ground (copied from the block before `m_renderGraph.execute`), and the descriptor table the
   depth draw reuses (`(setNext - 1) % kSets`: on the very first frame no colour draw has written one; the vertex stage reads only
   the landscape's own images, which every table has). Run it with the debug layer on.
4. When 2 and 3 hold on the GPU: delete the switch (`landscapeD3dPassesOn` in `LandscapeRenderFrame.h`, its three uses) so the
   mask is always set. If one of them does NOT hold and cannot be fixed at once: make the Windows default `false` there, so D3D
   users are back on the old behaviour.
5. Vulkan on a real GPU: `landscape_gpu_check --only vulkan` (shadows and, off macOS, the SSAO pass are on; expect the
   `the ground casts` line as on MoltenVK). For SSAO the line `ambient occlusion: N ground px are darker ...` must
   be above 60 in the forward run. On MoltenVK N is 0 although the patches are drawn into the position target (pipeline built,
   140-200 patches a frame); the forward frame there also draws the test box black, so I could not tell a wrong position from a
   capture that is off. Code: `VulkanLandscape::drawDepth(..., viewForPosition)`, `runSSAO`.
6. The lavapipe CI job does not run `landscape_gpu_check`; adding it there would give a second Vulkan and, with Mesa's software
   OpenGL, possibly the first run of the OpenGL material path (it needs `GL_MAX_TEXTURE_IMAGE_UNITS` >= 24). Not done.

**Gaps to fill, largest first** (each has its Metal reference in `MetalRenderer.mm`)

* **D3D11 / D3D12 draw the ground with the fixed blend, not the material.** The shaders exist
  (`MaterialShaderLibrary::landscapeVertex / landscapeFragment` with `Backend::HLSL`). Missing is the C++: compile the pair for a
  material hash (beside `GetOrBuildMaterialShaders` / `GetOrBuildMaterialPSO`), and a draw that binds what a mesh draw of that
  material binds plus the landscape's data at the registers above. The block layouts are the GLSL ones, i.e.
  `HE::vkls::Globals` / `PatchPush` (`VulkanLandscapeLayout.h`, plain C++), NOT `d3dls::Globals`: a second constant buffer.
  D3D12 needs b12/b13 and t47..t55 in the material root signature. Reference: `MetalRenderer::BindLandscapeMaterial`,
  `GetOrBuildLandscapeMaterialPipeline`; Vulkan's `lsDraw` hooks in `drawDCVk`. Then set `Capabilities::landscapeMaterial`.
* **G-buffer on Vulkan / OpenGL / D3D**: the material's G-buffer fragment with the landscape vertex (Metal:
  `EncodeLandscapeGBuffer`). Until then a deferred frame draws the ground forward on top.
* **GI** (Vulkan `runGi` raster, D3D, OpenGL 4.3): the ground in the GI pre-pass and in the ray structure (Metal:
  `EncodeLandscapePass` kind 1, `m_giLandMesh` in `EncodeGIAccelBuild`).
* **SSAO** on OpenGL and D3D, **velocity** everywhere but Metal (Metal: `EncodeLandscapePass` kinds 2-4).
* **Virtual texture** (`landscapeRvt`), section 7b.

## 9. The Windows run of 22.11. (NN-WS03, RTX 4070, Debug build, branch with `claude/landscape-d3d-passes` merged in)

All four `landscape_gpu_check --only <backend> --out <dir>` runs exit 0 (without `--arrays`: the Forest array is not on this machine,
the tool's "world-uv ground from the Forest array" scene fails with it for that reason alone).

| | Result |
|---|---|
| `he_tests --source-file=*test_landscape*,*test_material_graph*,*test_game_build_config*` | 334 cases, 0 failed (incl. `test_landscape_physics.cpp:298` and the D3D shader contract); run before the fixes below |
| FXC (`fxc /T vs_5_0`, `ps_5_0`, entry `main`) on `landscape_vs.hlsl` / `landscape_ps.hlsl` | both compile; warnings only (X3556 integer divide, X4000 `heLsTexel` possibly uninitialised) |
| D3D11 shadows (section 8, step 2) | **proven**: 141/255 against 203 (forward), 150 against 222 (deferred); with `HE_LANDSCAPE_D3D_PASSES=0` 203 = 203 |
| D3D12 shadows (step 3) | **proven** by the same numbers. The debug layer is **not installed** here ("Graphics Tools" feature), so the PSO format, root state and descriptor table were judged by the picture only |
| D3D11 / D3D12 `coarseRevision` (item 2a) | **built and proven**: `the move uploaded 30 tile(s) of the 191`, 0 of 129600 px apart, far stroke 57 px. `D3D11Landscape::upload` / `D3D12Landscape::upload` (`fresh` against a changed revision), `landscapeCoarseRevision = true`, `refreshesCoarse()` |
| Vulkan (step 5) | **proven**: shadows 140 against 203, `ambient occlusion: 57739 ground px` (forward), 57570 (deferred): the SSAO pass holds on a real driver |
| OpenGL 4.6, ground drawn with its material (first run anywhere) | failed, **fixed, proven**. Forward was black: the lighting block is uploaded by the first graph-material mesh only, and the scene had none. And the ground took no sun shadow: the mesh materials' fill has no cascades. `OpenGLRenderer.cpp`, the block before `DrawLandscape(viewProj, landscapeLightUbo)`: 6894 px (forward) / 4631 px (deferred) in the box's shadow |

Decided: the two switches (`landscapeD3dPassesOn`, `landscapeVkSsaoOn`) **stay**, on by default as before. Step 4 said to delete the D3D
one when 2 and 3 hold; they hold in the picture, but D3D12 never ran under the debug layer. Delete it after one such run.

Fixed on the way: `D3D12Landscape::drawDepth` called `fillGlobals` without step 28's `farBound` argument (the two branches met here).

## 10. Second Windows session of 22.11.: the material and SSAO on D3D, the Mountain generator

The table of section 8, as it stands now (NN-WS03, Debug, `landscape_gpu_check --only <backend>`, exit 0 on all four):

| | Vulkan | OpenGL 4.6 | D3D11 | D3D12 |
|---|---|---|---|---|
| Ground drawn with its material | proven | proven | **proven**: 115250 px forward, 109033 deferred | **proven**: the same numbers |
| Ground takes sun shadow / fog | proven | proven | **proven**: 4561 / 4551 px, fog 65.7 / 68.6 of 255 | **proven**: the same |
| Ground casts into the shadow layers | proven | proven | proven | proven |
| Ground in the SSAO pre-pass | proven | not built | **proven**: 62577 / 62453 px | **proven**: the same |
| GI (pre-pass, ray structure) | not built | not built | not built | not built |
| G-buffer (deferred) | not built (forward on top, lit) | not built | not built | not built |
| Velocity (TAA) | not built | not built | not built | not built |
| Virtual texture (`landscapeRvt`) | not built | not built | not built | not built |

* **How the D3D material draw works** (the Vulkan `lsDraw` pattern): `drawDC` / `drawDC12` with `lsDraw` set gets a draw call that
  names the ground's material; the shaders are `GetOrBuildLandscapeMaterialShaders` (D3D11) / `GetOrBuildLandscapeMaterialPSO` (D3D12),
  every bind and restore is the mesh draw's, and `D3D11Landscape::drawPatches` / `D3D12Landscape::drawPatches` add b12 / b13
  (`HE::vkls` layout) and t47..t55 and draw. D3D12 has `m_lsMatRootSig` (`DescribeLandscapeRootSignature`,
  `D3D12MaterialRootSignature.h`) and puts the nine views into the next block of the frame's material SRV ring. A scene with nothing
  but the ground no longer leaves `DrawScene` early (`groundNeedsPass`). The call site: search `lsDraw = &lf`.
* **SSAO on D3D**: `kLandscapeHLSL::PSViewPos`; the view matrix rides in the first four layer colours (`d3dls::setViewRows`), so the
  block and the root signatures keep their layout. `drawViewPos`, called from `runSSAO` through `drawGround`. Not in the three-target
  reflection pre-pass: with SSR on the ground takes no ambient occlusion (as on Vulkan).
* **Mountain generator** (`TerrainGenerate.h`): ridges, thermal erosion, terraces, paths routed from the summit and cut in as benches,
  returned in `Result::paths`; the Landscape mode's Mountain tool, the tile brush and MCP `terrain_mountain` take them, both tools can
  make a Spline entity "Mountain Path" per path. All off by default in `Params`; the two editor tools start with ridges 0.6 and
  erosion 0.4. The path search needs a cost per TURN (a state is node and direction), else it zigzags every step and the smoothing
  draws a straight line up the hill.
* **Tests**: `test_terrain_generate.cpp` (13 cases), `test_mcp_tools_terrain.cpp`, the D3D12 landscape PSO on WARP with its negative
  control (`test_material_graph.cpp`, "a material's tile landscape variant builds against the landscape root signature").
* **Tried and stopped**: two subagents (Mountain, Vulkan passes) were started and stopped before either changed a file; the empty
  worktree `C:\hw326v` (branch `claude/landscape-vulkan-passes`, at `1cb053dfa`) is theirs and can be removed.

## 11. Third Windows session of 22.11.: G-buffer, GI and SSAO on every Windows backend

The table once more (NN-WS03, Debug, `landscape_gpu_check --only <backend>`, exit 0 on all four). The owner's order: every gap on
the backends first, the merge of `release/0.8.0` after; origin is pushed after each step.

| | Vulkan | OpenGL 4.6 | D3D11 | D3D12 |
|---|---|---|---|---|
| Material, sun shadow both ways, fog | proven | proven | proven | proven |
| SSAO pre-pass | proven (57739 px) | **proven**: deferred 56170 px (from the G-buffer's depth), forward 116 px | proven (62577) | proven (62577) |
| G-buffer on a deferred frame | **proven**: 108568 of 110178 px | **proven**: 108216 of 110765 | **proven**: 108365 of 109031 | **proven**: the same |
| GI: the ground TAKES the ray-traced shadow | **proven**: 5514 px | deferred **proven** (4913 px), forward faint: NOT claimed | **proven**: 5303 px | **proven**: 5303 px |
| GI: the ground CASTS it | **proven**: 145/255 against 204 | proven on both paths (110 against 217), not claimed | **proven**: 145 against 204 | **proven**: 145 against 204 (DXR) |
| Velocity (TAA) | not built | not built | not built | not built |
| Virtual texture (`landscapeRvt`) | not built | not built | not built | not built |

* **New witnesses in the tool** (`EngineCheck.cpp`): `G-buffer: the normal view differs on N of M ground px` (the pass was only
  reported before), `no self-shadow, GI on: N of M ground px` (the clipmap in the ray structure against the drawn mesh).
* **The height clipmap never occluded anything on D3D11, Vulkan and OpenGL** although the GI doc of 02.11. says so: all three
  used it only with a caster list of its own (`sep`), which a `TerrainComponent` has (its chunks are left out) and the clipmap
  never. And every kernel lifted a ray start onto the field, so nothing under the ground was in its shadow: capped at 8 m in all
  five copies (`kGiHeightfieldHLSL`, `gi_shadow.comp`, `gi_shadow_hw.comp`, `gi_shadow_hw.hlsl`, the GLSL in `OpenGLRenderer.cpp`).
* **Hardware ray tracing**: the ground is no mesh, so it is in no TLAS. `gi_shadow_hw.comp` (Vulkan) and `gi_shadow_hw.hlsl`
  (D3D12 DXR) march the clipmap after their ray query. D3D12 had no height field at all: `ensureGiHeightfield`, root parameter 10
  (b2), heap slots 46..49.
* **The pre-pass stages** (position + normal for the GI mask, view position for SSAO): `PSGiGBuf` / `PSViewPos` in
  `kLandscapeHLSL` (D3D), `fsGiGBuf` in `VulkanLandscape`, `DrawLandscapeGiGBuf` / `DrawLandscapeViewPos` on OpenGL (the material
  library's landscape vertex stage with a fragment of three lines, drawn through `OpenGLLandscape`'s material path).
* **`RenderExtractor::extractLandscapeGiHeightfield`** keeps the clipmap entry current on a reused walk and follows
  `HE_LANDSCAPE_PASSES`.
* **Trap that cost five runs**: `landscape_gpu_check.exe` loads `HorizonRendering.dll` and `Shaders/*.cso|*.spv` from its own
  folder, and they are only copied there when the exe relinks. After an engine-only edit: copy them
  (`out/build/x64-debug/src/HE_Rendering/HorizonRendering.dll`, `out/build/x64-debug/Shaders/`) before believing a result.

## 12. Fourth Windows session of 22.11.: GI on OpenGL forward, the virtual texture on all four

* **OpenGL forward GI** (`c05e75a92`): the ground's forward draw ran before any mesh material draw had bound units 14 to 17 (the
  GI shadow mask and its neighbours), so it read whatever was there. `OpenGLRenderer::DrawScene` binds them before
  `DrawLandscape`; OpenGL claims `kLandscapePassGi` now.
* **The virtual texture on the tile ground** (section 7b), `Capabilities::landscapeRvt` on D3D11 (`ada4667c4`), D3D12
  (`b2d17d7a8`), Vulkan (`7d9b6956f`), OpenGL (`3862c05bb`). One shape everywhere: `SetRvtTileGround` stores consumer + rectangle
  in `m_tileRvt*`; the ground's material draw (`drawGroundMaterial`, on OpenGL `DrawLandscape`) splits the frame's patches with
  `HE::splitRvtTilePatches` (`HorizonRendering/RvtTilePatches.h`) and draws the inside ones with the consumer; a page with
  `rvtTileGround` draws the ground with `rvtMaterial`, a page of meshes leaves the ground out.
* **Evidence** (`--arrays`, `HE_LGC_GROUP=mountains`, forward and deferred, all four backends the same within 0.04/255): pages
  0.00 from the ground computed in place; a stamp 4.2 km away shows (0.00 to 0.04 against 22 to 30 for the pages from before
  it); 93 pages stale and painted again after a tile is re-read; floating origin moved 1 km: 0.02 to 0.03; the camera-centred
  volume swaps once over 2.5 km and never falls back. Without `--arrays` every backend exits 0.
* **What `--arrays` cannot show on this machine**: only the Mountains arrays are here, and that group paints uniform snow, so
  "the pages are what is drawn (the picture changes when the swap is paused)" and "the ground has texture in it" fail on all
  four; with the default Forest group the arrays are missing and the pages differ from the in-place ground by 10 to 14/255 on
  all four (the page material and the in-place fallback are not the same picture without textures). Neither is a backend
  difference. A machine with the Forest arrays has to run it once.
* **The `--arrays` root here**: `C:\ProgramData\Hive\w\lgc-content`, junctions (PowerShell `New-Item -ItemType Junction`, bash
  `mklink` made broken ones) to the deploy's `EngineContent\{Materials,MaterialFunctions,Meshes,Particles,Widgets}` and
  `%APPDATA%\HorizonEngine\EngineContentCache\Textures`. `HE_LGC_GROUP=mountains` is `checkGroup()` in `EngineCheck.cpp`.
* **`he_tests` after these commits** (Debug, scratch APPDATA, 4073 cases run): 3 fail, none in files this branch touches.
  `test_inspector_ui.cpp:510` (clipboard, known), `test_hc_codegen_export_build.cpp:181` (empty build message; run outside
  vcvars, not looked into), `test_physics_threading.cpp:305` crashes with an SEH exception every time (fixed in section 13).

## 13. Fifth Windows session of 22.11.: the Debug physics crash, velocity on all four

* **`test_physics_threading.cpp:305`** (`360be5ed6`): Jolt is built with asserts in Debug and its default `AssertFailed` breaks
  into the debugger. `PhysicsSystem::Update` asserts on a full contact cache right before it returns the error
  `PhysicsWorld::step` counts, so every Debug process died there. `joltEnsureInit` (`PhysicsWorld.cpp:192`) installs a handler
  that logs and carries on. `--source-file=*test_physics*`: 134 cases pass. Not a landscape change; `release/0.8.0` has the
  same crash until this branch is merged.
* **Velocity (TAA)**, `kLandscapePassVelocity` claimed on D3D11 + D3D12 (`b15e66849`), Vulkan (`4583a9b9a`), OpenGL
  (`ade0b4fc9`). The ground is drawn FIRST in each backend's velocity pass, with the scene's jittered matrix pulled a hair
  towards the camera (`vkls::velocityClip`, `VulkanLandscapeLayout.h`): the depth it tests against was written by the
  MATERIAL's vertex stage, which is other code; the pass writes no depth, so the meshes drawn after it overwrite it.
  D3D: `PSVelocity` in `kLandscapeHLSL`, the two unjittered view-projections as rows in `layerColor[0..3]` / `[4..7]`
  (`d3dls::setViewRows(g, m, first)`), `D3D11Landscape` / `D3D12Landscape::drawVelocity`. Vulkan: `fsVelocity` (`uAux0` /
  `uAux1`, both with this frame's jitter like the meshes), `drawDepth(..., velocityPrev)`. OpenGL: `DrawLandscapeVelocity`,
  `OpenGLLandscape::Shading::depthReadOnly` (the pulled-in depth must not be written: at 1 km it is 20 m nearer).
* **The witness** (`EngineCheck.cpp`, "TAA, live viewport"; needs `--arrays`, any group with texture: Forest without its arrays
  works). A still resets the TAA history on all four Windows backends, so the old contrast lines say nothing. The pan now also
  runs through `SetEditorCamera` + `Render()` + `CaptureViewport` and is compared with the camera STANDING at the pan's last
  position, as 4x4 block averages on the ground (ghosting, not texel noise); the same again with `HE_LANDSCAPE_PASSES=0`.
  Fails a backend that claims the bit when "with" is above 0.6 x "without". Measured, forward and deferred:
  D3D11 2.20 / 5.73, D3D12 2.20 / 5.73, Vulkan 2.14 / 5.60, OpenGL 1.53 / 4.05 (/255, with / without the ground's velocity).
  Metal runs this block too now and was NOT run: if `Render()` gives no image there it prints NOT CHECKED, it cannot fail a
  correct backend.
* **Not covered**: motion blur was not looked at.

## 14. Sixth Windows session of 22.11.: velocity across a floating-origin hop, occlusion with reflections on

* **The origin hop.** Last frame's view-projection is in last frame's origin; what THIS frame places (the ground, a foliage
  cluster's plants, an object seen for the first time) is relative to this frame's. `HE::taaPrevViewProjAt`
  (`TemporalAA.h`) puts the origin's step into that matrix. Each renderer keeps `taaPrevOrigin` next to `taaPrevViewProj`
  (advanced together at the end of the velocity pass, from `RenderWorld::worldOrigin`) and builds `prevHere` once per pass:
  the ground, the `isCluster()` branch and the first-seen branch take it; an object with a remembered model keeps the raw
  matrix (`shiftWorldOrigin` moved the entity, so model and matrix are both in the old frame; the corrected one would shift
  it twice). D3D11 `renderTaaVelocity`, D3D12 the velocity block in `DrawScene`, Vulkan `encodeTaaVelocity`, OpenGL
  `RenderVelocity`. Unit test: `test_temporal_aa.cpp`, "taaPrevViewProjAt" (both paths, and the double shift).
  Witness "the floating origin hops by (1.5, 0.8) m under a standing camera" (live viewport, one frame after the hop against
  the settled picture): the ground 0.30 to 0.45/255 on all four, 4.55 and a failure with the correction off (D3D11); the box
  (remembered model) 0 blocks over 6/255, 27 and a failure with the step applied to it too (D3D11 deferred; forward shows 0
  either way, so that half only bites on deferred). NO GPU witness for clusters or first-seen objects (the tool has no
  foliage): the unit test and the shared `prevHere` are the evidence. Metal has the same gap (`MetalRenderer.mm` velocity
  pass, its cluster branch and `EncodeLandscapePass`), not touched: open for the Mac.
* **Occlusion with screen-space reflections on.** A forward frame's position pre-pass is then the three-target reflection
  pre-pass, and the ground was left out of it. Now in: D3D `PSViewPosMrt` (`drawViewPos(..., mrt)`; D3D12 a second PSO with
  the RGBA16F / RGBA16F / R32F targets), Vulkan `fsViewPosMrt` (`drawDepth(..., viewPosMrt)`), OpenGL one fragment for both
  shapes (three outputs; a pass with one draw buffer drops two). Written like `kReflPrepassFS` writes a mesh: view position,
  octahedral world normal with roughness 0, depth.
  Witness "ambient occlusion, reflections on" (forward, no `--arrays` needed): D3D11 62577 px, D3D12 62577, Vulkan 57742,
  OpenGL 56709; with the ground kept out of the three-target pass D3D11 reads 0 px and fails.
  The reflections themselves on the ground (what the trace does with the ground's normal and depth) were not judged.
* Vulkan prints one new validation WARNING in these runs, `Vertex attribute at location 2 not consumed by vertex shader`: the
  renderer's own mesh pipeline of the reflection pre-pass (the ground's pipelines have one attribute), visible now because
  the check turns reflections on. Not touched.

## Handoff

- **Done / verified / not verified:** sections 9 to 14 for Windows; the three docs named at the top for the Mac. Not verified on
  Windows: D3D12 under the debug layer (not installed) and its SOFTWARE GI kernel (this machine takes DXR), a Release build,
  `--arrays` with the Forest arrays (section 12), the editor by hand (section 5; also the Mountain rows and the path splines).
- **Open, in this order:** (1) On the Mac: `HE::taaPrevViewProjAt` into Metal's velocity pass (section 14), then the new
  witnesses (TAA live viewport, origin hop, occlusion with reflections on). Then `release/0.8.0` into this branch (PR 233
  among its commits).
- **Do not:** bump the landscape's generation for an edit (it empties every backend's atlas); rely on `LandscapeTileSource::coarse()`
  (null for a pack in coarse regions: `coarseWindow`); name anything `far`, `near` or `small` (Windows macros).
- **Order:** 0, 1, 2, 3, then 5. Items 2 and 3 are board steps 27 and 28; item 1 has none yet.
