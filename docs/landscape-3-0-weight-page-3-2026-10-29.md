# Landscape 3.0 follow-up: a third weight page, 12 paintable layers (Thema 329, step 1)

Status: **code, tests and the four landscape `.hasset` files done on the Mac; D3D/Vulkan only read and cross-compiled,
not run.** Read the Handoff at the end.

## What changed

* `kTerrainWeightPages` 2 -> 3, `kMatMaxLandscapeLayers` 8 -> 12 (`TerrainComponent.h`, `MaterialGraph.h`).
  `TerrainComponent::layerWeights3` holds layers 8..11 (R=8 .. A=11), same size rule as `layerWeights2`; it never exists
  without a second page beside it, and is uploaded only once a byte in it is set.
* Weight texture: still ONE texture on binding 14 / t14 / unit 13 / Metal texture 13. 1 page = `weightRes` square (the
  bytes every landscape uploaded before there were eight layers), 2 pages = `2*weightRes` x `weightRes`, 3 pages =
  `3*weightRes` x `weightRes`, rows `[p0 | p1 | p2]` (`TerrainPaint::buildWeightTexture`, `weightPageCount`).
* Shader (`MaterialGraph.cpp` `emitLayerBlendNode`): the page count is read from the texture's own size,
  `np = max(floor(ts.x / max(ts.y, 1) + 0.5), 1)`, instead of the old `ts.x > 1.5 * ts.y` test with fixed `* 0.5` /
  `+ 0.5`. Page offsets are `k / np`. For `np == 2` every divide is by a power of two, so the 2:1 and square reads are
  bit-for-bit the old ones; a blend that names a ninth layer also reads page 2 and selects it with `np > 2.5`.
  `kChan` gained `_w3.x .. _w3.w`. One GLSL text for all five backends (Metal/HLSL/SPIR-V come from it by
  cross-compilation), so no backend file changed.
* `TerrainPaint`: `paint()` (page pointers, `nAll` 4/8/12, page 3 allocated by the first stroke on layer 8+ together
  with page 2), `fillLayer()` (both paths clear or keep page 3 like page 2), `texelWeights` (`out[8..11]`),
  `ensureWeightmap` (a third page without a matching second page is dropped). Shares 6/7 (group reserve) are untouched.
* `TerrainSystem::computeAverageLayerWeights` averages all three pages (`avgLayerWeights` is 12 floats).
* Scene file: `layerWeights3B64`, written only beside `layerWeights2B64` and only when the third page exists; read only
  when a second page was read. A scene without the key (every scene saved so far) loads as before and uploads the same
  bytes.
* Collab: `TerrainSync::Weights3 = 4` (the retired foliage-mask id), in `forEachGrid`, `viewGrid`, `reallocateGrid`,
  `markWritten`, `GridsAside`, `restoreTerrain`. A landscape with no third page sends nothing for it. Protocol
  `kCollabProtocolVersion` 17 -> 18 (an older peer drops the new field, which the version rule calls a bump).
* MCP: `terrain_paint` takes `layer` 0..11, `mixAtCenter` has 12 entries, writes `layerWeights3B64`;
  `terrain_info.layerAverage` has 12 entries.
* Editor: Clear Paint clears page 3, the foliage "Paint Layer" combo lists layers 8..11, the Material Editor says
  "12 layers max", `HE_DUMP_LANDSCAPELAYERS=12` is a Metal/GL witness with four more discs.
* The four shipped landscape materials (`M_AutoLandscape`, `_Forest`, `_Grassland`, `_Desert`) are regenerated with
  `landscape_tex_gen <EditorDeps/EngineContent/Materials> --material [--group G]`, because their baked GLSL changed.
  `Water.hasset` does not sample the weightmap and is unchanged.

## Handoff
