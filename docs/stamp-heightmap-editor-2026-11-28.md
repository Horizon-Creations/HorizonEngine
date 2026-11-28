# Stamp Heightmap window (2026-11-28)

Window > Stamp Heightmap paints the heightmap file of a Landscape Stamp (`LandscapeStampComponent::heightmapFile`).

## Handoff

**What exists**
- `src/HE_Editor/StampHeightmap.{h,cpp}`: the picture (floats 0..1) and its strokes, no ImGui: `dab` (raise/lower), `smooth`,
  `line` (height runs from start to end, fades to the sides), `gradient` (transition over the whole picture), `writePgm16`, `read`.
- `src/HE_Editor/StampHeightmapPanel.{h,cpp}`: the window. Controls left, canvas right. Saves `<project>/Heightmaps/<Name>.pgm`
  (16 bit). "Assign to Selected Stamp" and "Use in Stamp Tool" (`LandscapeWorldTools::setStampToolFile`) hand the file on.
- Live preview: after Save or Load, every stroke rewrites the file (at most ~7 times a second during a stroke) and calls
  `AppContext::reloadLandscapeStamps` (= `LandscapeStampSync::reset()`, wired in `EditorApplication.cpp` next to `.rvtDebugInfo`).
- Canvas: `IRenderer::CreateImGuiTexture`, old texture destroyed one frame later; a backend that returns null (Software) gets
  64 x 64 flat cells instead.
- Registered in `EditorUI.cpp` (toggle, panel pref `PanelOpenStampHeightmap`, Window menu, draw), `MacMenuBar.{h,mm}`,
  help entries `Stamp Heightmap/*` and `Window/Stamp Heightmap` in `EditorHelp.cpp`.

**Verified**
- `he_tests --test-case="stamp heightmap*"`: 3 cases green (line, transition, brush, PGM round trip through `readGrey16`). Release, MSVC.
- `editor_help_audit.py --check`: green.
- Deployed Release editor, hidden, private APPDATA, panel open by pref, CPU-rastered UI shot (`HE_DUMP_UI`): OpenGL, Vulkan,
  D3D11, D3D12 each show the window with a texture on the canvas (flat grey in the shot = a GPU texture, not the fallback).

**Not verified**
- No stroke was drawn with a real mouse, and the live preview on a landscape was not watched: both need a person at the editor.
- Metal (`MacMenuBar.mm` edit not compiled here) and the Software fallback.
- No headless UI test of the panel (`tests/test_*_ui.cpp` style).

**Known limits (marked `shortcut:` in code)**
- A live write reloads every stamp's picture, not only the edited file's.
- The texture of the window is not released at shutdown; undo is the window's own (button, 32 steps, no Ctrl+Z, no redo).

**Next steps, if wanted**
1. Noise / erosion tools, flatten brush, redo.
2. Per-file reload in `LandscapeStampSync` instead of `reset()`.
3. A `test_stamp_heightmap_ui.cpp` that drags on the canvas.
