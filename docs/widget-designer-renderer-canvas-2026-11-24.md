# Widget designer: the canvas drawn by the renderer (2026-11-24)

The designer canvas used to be an ImGui imitation of every widget type
(`drawElementPreview`, ~700 lines). Materials, Pixelated, fonts and every new
style property had to be imitated a second time and were not always. Now the
canvas is the active renderer's own picture of the runtime's quads.

## Handoff

**What was done**
- `HE::uiEmitElement` (`UIWidgetTree.h`, defined at the end of `UIWidgetTree.cpp`): the
  per-element emit lifted out of `WidgetManager::extract` without changes (drop shadow,
  `render`, uiState, surface stamp, opacity/dim, clip, rotation). Both the runtime and the
  designer emit through it. The focus ring, scrim, dropdowns and tooltips stay in the manager.
- `WidgetManager::resolveElementAssets` (static): texture id/size, font atlas key and
  material id for one element. `refreshElementAssets` uses it.
- `IRenderer::RenderUIPreview(quads, w, h, clear, slot)`: the game's UI pass into an
  offscreen target, returned as an ImGui handle. Metal (`MetalRenderer.mm`, before
  `RenderAssetThumbnail`) and OpenGL (`OpenGLRenderer.cpp`, before `RenderParticlePreview`;
  bottom-up, the caller flips V). Software, Vulkan and D3D return nullptr.
- `UIEditorPanel.cpp`:
  - `emitTreeQuads` builds the page (as a theme-applied copy) and every embedded WidgetRef,
    mirroring `drawEmbeddedTree`.
  - `drawCanvas` renders the visible canvas region at framebuffer resolution, with the grid
    as hairline quads underneath, and shows the image.
  - With `g_aidsOnly`, the ImGui replica then draws only the designer aids: box, grid and
    spacer bounds, slot dashes, ghost list rows, ref outline.
  - Without a preview path, the old replica draws everything, as before.

**Verified**
- `UITEST=pixelated RHI=Software`: md5 identical before and after the extract refactor.
- `test_widget_designer_ui` "the canvas is the renderer's picture of the runtime's quads":
  - the designer's quads equal `WidgetManager::extract` for the same widget at the same
    size, quad for quad (rounded, border, gradient, shadow, rotation);
  - the shot shows the rasterized picture.
- Full ctest 382/382 and `editor_help_audit --check`.

**Not verified**
- The Metal and OpenGL paths in the running editor. Both compile; no frame has been
  looked at, because there is no headless capture of the ImGui window.
- Pixel comparison between the designer canvas and the game frame on GPU.
- Backdrop materials in the designer (the pass gets cmdBuf and pass, but untested).

**Next steps**
1. Open a widget in the editor on Metal and on GL, and compare it with Play
   (materials, text, Pixelated).
2. For projects that ship on Software ("Advanced off"): optionally let the designer
   draw through `HE::sw::draw` so it matches the CPU rasterizer exactly (materials fall
   back to Schicht 0).
3. Vulkan/D3D: implement `RenderUIPreview` (until then they keep the replica).
4. Once the renderer path has been proven, the content half of `drawElementPreview` can go.
