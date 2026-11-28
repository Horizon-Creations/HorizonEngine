# Stamp Heightmap editor (2026-11-28)

An editor tab that makes the heightmap file of a Landscape Stamp (`LandscapeStampComponent::heightmapFile`): generate, sketch and
naturalize, erode, filter, with a test landscape beside the picture.

## Decisions

- **No new asset type (yet).** The document is the file a stamp already reads at 16 bits: `<project>/Heightmaps/<Name>.pgm`.
  A real asset would be a new `AssetType::LandscapeHeightmap` (16-bit payload) plus a `heightmapId` on the stamp component, its
  serialisation, the export bake and the Content Browser rows (about ten files, see how `BoneMask` is spread). Only suggested.
- **Ways in:** Window > Stamp Heightmap; "Edit Heightmap..." on a Landscape Stamp (Inspector) and on the Landscape mode's Stamp
  tool, both through `HE::StampHeightmap::requestEdit(file)`, taken up in `EditorUI.cpp` (search `takeEditRequest`).
- **Test landscape = an engine terrain.** A one-entity `HorizonWorld` with a `TerrainComponent` (129 x 129, `sculptHeights` from the
  picture), built by `TerrainSystem::updateTerrains` and drawn by `IRenderer::RenderWorldPreview`, the Mesh Viewer's path.
- **Destructive picture + undo**, no node graph and no layers. A painted selection limits generators and filters to a region.

## Handoff

**What exists**
- `src/HE_Editor/StampHeightmap.{h,cpp}` (no ImGui): strokes (`dab`, `smooth`, `flatten`, `line`, `gradient`); `generate`
  (Hills, Mountain, Mountain Range, Plateau, Crater, Volcano, Dunes, Island; gradient noise, fBm, ridged multifractal, domain
  warp; blend Replace/Add/Subtract/Max/Min); `refine` (the sketch-to-ground pass: blur, warp, height-scaled ridged relief,
  erosion); `erode` (droplet erosion after Beyer 2015 + thermal passes); `blur`, `terrace`, `normalize`, `invert`, `fadeEdges`;
  every generator and filter takes an optional mask; `writePgm16`, `read`; `requestEdit` / `takeEditRequest`.
- `src/HE_Editor/StampHeightmapPanel.{h,cpp}`: the tab (`kTabPath`, `render`, `open`, `save`). On top one `EditorToolbar::Bar`:
  File / Edit / View (popups), the modes Generate, Sketch, Naturalize, Erode, Filters, Select, then Undo / Redo, Save on the
  right. The left column shows only the options of the mode that is on (`drawModeOptions`); the picture is in the middle, the
  test landscape right. The mouse paints only in Sketch and Select. Undo/Redo 16 steps. Ctrl+S saves (`doSaveActiveTab`).
- Test landscape (third pass, after the owner's screenshot showed a burnt-out white sheet with too coarse a grid): 513 x 513
  grid (was 129), rebuilt at once after a button and ten times a second during a stroke; a material of its own
  (`kGroundAlbedo` 0.6 times a texture from `StampHeightmap::groundColours`: grass, soil, rock by slope, snow by height; View >
  Ground Colours off = even grey); the sun lower by default (`timeOfDay` 0.33).
- Live preview on the real landscape: after Save or Load every change rewrites the file and calls
  `AppContext::reloadLandscapeStamps` (= `LandscapeStampSync::reset()`).
- Registered: `EditorUI.cpp` (Window menu, tab dispatch, save, request), `MacMenuBar.{h,mm}` (`OpenStampHeightmap`),
  `InspectorPanel.cpp` and `LandscapeWorldTools.cpp` (the two buttons), help entries `Stamp Heightmap/*` in `EditorHelp.cpp`.
- `HE_DUMP_STAMPHEIGHTMAP=1`: a headless run opens the tab with a generated, naturalized mountain and writes
  `Heightmaps/Stamp.pgm` into the project.

**Verified**
- `he_tests --source-file=*test_stamp_heightmap.cpp`: 8 cases (strokes, every landform deterministic per seed and in 0..1, mask
  limits every generator and filter, naturalize, erosion keeps mass and is deterministic, filters, PGM round trip through
  `readGrey16`, the edit request). Also green: `test_landscape_world_ui`, `test_terrain_tools_ui`, `test_inspector_ui`. Release, MSVC.
- `editor_help_audit.py --check`: green.
- Deployed Release editor, hidden, private APPDATA, `HE_DUMP_STAMPHEIGHTMAP=1`, 3000 frames, on OpenGL, Vulkan, D3D11, D3D12:
  the tab opens (CPU-rastered UI shot), the log has "TerrainSystem: rebuilt 4 of 4 chunks ... field height 0 .. 64.6" and
  "test landscape drawn by the renderer's world preview", exit code 0.
- The generated mountain was looked at as a hillshade of the written PGM (ridges, gullies, no rim). That was before the
  settling blur at the end of `erode` was added; the picture after it was not looked at again.

**Not verified**
- The PIXELS of the test landscape: the UI shot rasters foreign textures as flat grey, so only "a texture came back and the
  terrain was built" is proven, not what it looks like. The owner saw the second pass on screen (D3D, white and burnt out);
  whether the material of the third pass cures that, whether the ground texture arrives at all and whether it lies the right
  way round on every backend has NOT been seen. If it is still too bright: lower `kGroundAlbedo`.
- Any stroke with a real mouse, camera navigation in the test landscape, the live preview on a real landscape.
- Metal (`MacMenuBar.mm` not compiled here); Software has no world preview and no ImGui textures (text + coarse cells instead).

**Known limits (marked `shortcut:` in code)**
- `erode` runs on the UI thread and caps at 300 000 drops: a 1024 picture blocks for a moment and is eroded more thinly.
- A live write reloads every stamp's picture, not only the edited file's.
- Brushes ignore the selection (generators and filters respect it). No Ctrl+Z in the tab (buttons only).

**Next steps, if wanted**
1. Look at the tab on screen on each backend; tune the generator constants by eye.
2. The asset type above, so heightmaps live in the Content Browser and ship without the bake being the only carrier.
3. Erosion on the job pool with progress; brushes that respect the selection; a noise brush; rivers along a drawn line.
