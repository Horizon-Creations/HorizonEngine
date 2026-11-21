# Tile landscape: the render window and a coarse world read from disk (Thema 326, 2026-11-21)

## Why

Three things grew with the world's AREA and were held whole: the coarse world (heights at 32 m, weights at 128 m: 27 MB at 100 km,
2.7 GB at 1000 km) on the CPU and in one GPU image, the far image, and the far image's table in memory. Tiles, physics, foliage,
navigation and water already stream around the camera. This step makes the far levels' data the WINDOW's, so a world's size is bounded
by its file, and reads that data from disk window by window.

## Built

| Part | Where | What |
|---|---|---|
| Render window | `LandscapeRuntime.cpp` (`placeWindow`, `buildFrame`), `LandscapeRuntimeConfig::renderWindowTiles` (256 = 131 km) | A world with more tiles a side is handed to the renderer as a window around the camera, in window coordinates: the quadtree's world is the window, `origin` and the camera are moved by the window's corner, the atlas is asked with the corner added. **No shader, no image layout, no backend code changes.** A world that fits has its corner at 0 and is drawn exactly as before |
| Window moves | `placeWindow` | In steps of the slot table (64 tiles), when the camera leaves the middle band (hysteresis). The corner is a multiple of the table's size, so a tile's table entry is the same in both spaces. To a renderer a move is another landscape: new generation, the atlas begins again |
| Source | `LandscapeTileSource::coarseWindow`, `landscapeCoarseCrop` | The far data of a window. Default: a crop of `coarse()`. A source may have no whole coarse world at all |
| Texture phase | `LandscapeRenderFrame::uvOrigin`, four one-line changes (GL, Metal, `VulkanLandscapeLayout.h`, `LandscapeD3D.h`) | The world-UV phase keeps the absolute floating origin, so textures do not jump when the window moves |
| Pack on disk | `LandscapeSource.h/.cpp`: section `'HLCR'`, `LandscapePackWriter::setCoarseRegions`, `LandscapePackSource::coarseWindow / coarseRegionTiles / coarseHeightLevel` | Worlds over 256 tiles a side store the coarse world in regions of 64 x 64 tiles with an index; opening the pack reads the index only, a window reads the regions it touches. Smaller worlds keep the whole coarse world as every pack before (v2 unchanged). The section is part of the pack's core (`coreSize`), so rewriting the tail sections keeps it |
| Far image | `LandscapeFarRegion::blob`, `LandscapeFarTable::regionPixels / compact`, `assemble(window)`, `planLandscapeFar` | Regions stay stored and are decoded when a window is assembled; a bake compacts each region after painting it; the resolution is planned for a window, not for the world |
| Edits | `LandscapeEditSource::coarseWindow` | On a pack without a whole coarse world the window is patched with the edited tiles inside it |

## Verified (Mac, Apple M5, Release)

* `tests/test_landscape_window.cpp`, 6 cases: crop = the whole world's own samples on every grid; far image window = the whole image's
  texels; a **512 km** formula world (no coarse world of its own) drawn as a window, with origin, camera, patches, coarse data and slot
  table entries checked, a 10 km move that moves nothing, a 40 km move that moves the window, and the world's far corner; window sizes
  that do not fit the slot table refused; a pack in coarse regions = the whole coarse world, window by window, with tail sections
  rewritten behind it and a damaged region refused; a runtime and an edit on such a pack, and baking edits into it.
* Every landscape test from before, unchanged: 192 / 192 (that half is "a world that fits is drawn exactly as before").
* `landscape_gpu_check`, exit 0 on all Mac backends. Scene 6: a 164 km world drawn as a window that has **just moved** against the same
  world drawn whole: **0 of 129 600 pixels** more than 2/255 apart on OpenGL, Metal and Vulkan (MoltenVK). The backend comparisons of
  the older scenes print the same numbers as before the window existed (ramp 0, hills 18, world-UV 55).

## Not verified / not done

* **D3D11 / D3D12**: the `uvOrigin` line is the only change there and nothing ran. A window move on those backends takes the same
  "new generation" path a new landscape takes, which they have always had; not run for this.
* **A window move pops**: the atlas begins again, so for about 25 frames the near ground is drawn from the far levels. Once per 32 km
  travelled, only on worlds larger than the window. Marked `shortcut:` in `placeWindow`.
* **No real pack larger than the window was written or flown.** The 512 km world is a formula source; the region format is tested on
  small worlds with regions of 2 tiles. Nothing was timed.
* **Still whole in memory for a huge world:** the pack's tile index (12 B a tile: 46 MB at 1000 km), the cell HLOD table, the pack
  writer's coarse world until `finish()` (`shortcut:` in `LandscapeSource.h`), and a far bake's encoded table before it is written.
* The editor's streaming view and `LandscapeWorldTools` were not looked at for worlds larger than the window.

## Handoff

- **Topic / step:** 326, "streamed coarse world" (owner asked directly; no board step).
- **Branch / commits:** `claude/landscape-weltmassstab-100km`: `849eb8c56` (render window) and the commit after it (disk, far image, doc).
- **Decisions taken:** a window in window coordinates instead of clipmap windows in the shaders (those need new uniforms and upload
  paths on five backends, three of them not verifiable here); window = 4 slot tables, step = 1; regions of 64 tiles = the step; the
  coarse regions are core, not a tail section; old packs are not converted.
- **Traps:** the window's corner must stay a multiple of `atlasWindow`; anything that turns a frame position into an absolute one must
  add the corner (`LandscapeFrame::windowTile`), and only the texture phase may use `uvOrigin`; `LandscapeRuntime::quadtree()` now
  describes the window (its `tilesX/Z`), not the world; `coarse()` may be null, ask `coarseWindow` / `LandscapeRuntime::windowCoarse()`.
- **Next steps:** 1. Keep the atlas across a window move (GL/Metal first: coarse and far images of another size without a new
  generation), which removes the pop. 2. D3D run of scene 6 on NN-WS03. 3. A real pack over 256 tiles: build, fly, time. 4. The cell
  HLOD table and the writer's coarse world in regions.
