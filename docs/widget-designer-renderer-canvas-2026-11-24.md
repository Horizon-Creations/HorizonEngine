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
- `test_widget_designer_ui`, with a stub renderer whose `RenderUIPreview` is the software
  rasterizer:
  - "the canvas is the renderer's picture…": the designer's quads equal
    `WidgetManager::extract` quad for quad (rounded, border, gradient, shadow, rotation).
    At framebuffer scale 2 (Retina) the target and quads are twice as large.
  - "an embedded component…": a ref with a parameter, a page child in a NamedSlot and a
    ref under a clipping panel all equal the runtime graft quad for quad.
  - Pre Construct shows its green on the renderer path too.
- Metal and OpenGL: `HE_UIPREVIEW_DUMP=<ppm> scripts/he_shot.py OUT UITEST=1 RHI=<backend>`
  writes the `RenderUIPreview` target of the witness sheet. Tile interiors against the
  game frame: 1716 samples, maximum difference 0 on both backends.
- Full ctest 382/382 and `editor_help_audit --check`.
- Missing assets: a missed path is left out for 2 s (`s_missedAt`), so the log does not
  fill 60 times a second.

**Not verified**
- The designer image in the running editor window (the ImGui window cannot be captured
  headless). The GL V flip follows the convention of `RenderWorldPreview`.
- Backdrop materials in the designer (the pass gets cmdBuf and pass, but untested).

**Software apps (added the same day)**
- Rule: `appProject && !advancedShaderEffects` (the same one `BackendRules::forExport` uses
  to ship `"Software"`). The designer then draws through `softwareCanvas`
  (`UIEditorPanel.cpp`):
  - a `SoftwareRenderer` of its own from `RendererFactory`, with the new
    `IRenderer::RasterizeUI` (same texture decode and mips as the app);
  - the result uploaded via `CreateImGuiTexture`, top-down, with no V flip on any backend.
- Rasterized and uploaded only when the quads change (`UIRenderObject::operator==`,
  defaulted) or once a second. Old textures are released 4 frames later.
- `tests/WidgetDesignerLinkStubs.cpp`: `RendererFactory` for he_tests (Software only).
- Test "an app on the software renderer gets the CPU picture": no GPU preview call, an
  upload containing the app frame's gradient colour, and no re-upload without a change.
- Also works on Vulkan/D3D (only `CreateImGuiTexture` is needed). Not looked at in the
  running editor.

**Next steps**
1. Open a widget in the editor on Metal and on GL, and compare it with Play
   (materials, text, Pixelated). Do the same for an app project with Advanced off.
2. Vulkan/D3D: implement `RenderUIPreview` (until then games keep the replica there).
3. Once the renderer path has been proven, the content half of `drawElementPreview` can go.
