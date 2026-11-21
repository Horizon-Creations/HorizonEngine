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

## Handoff

- **Done / verified / not verified:** see the three docs named at the top; on Windows nothing is verified.
- **Do not:** bump the landscape's generation for an edit (it empties every backend's atlas); rely on `LandscapeTileSource::coarse()`
  (null for a pack in coarse regions: `coarseWindow`); name anything `far`, `near` or `small` (Windows macros).
- **Order:** 0, 1, 2, 3, then 5. Items 2 and 3 are board steps 27 and 28; item 1 has none yet.
