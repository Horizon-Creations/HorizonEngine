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

## Handoff

- **Done / verified / not verified:** see the three docs named at the top; on Windows nothing is verified.
- **Do not:** bump the landscape's generation for an edit (it empties every backend's atlas); rely on `LandscapeTileSource::coarse()`
  (null for a pack in coarse regions: `coarseWindow`); name anything `far`, `near` or `small` (Windows macros).
- **Order:** 0, 1, 2, 3, then 5. Items 2 and 3 are board steps 27 and 28; item 1 has none yet.
