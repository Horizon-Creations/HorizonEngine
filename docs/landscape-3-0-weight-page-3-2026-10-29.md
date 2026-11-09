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

- **Topic / step:** 329 (Landscape 3.0 Folge), step 1 of 5
- **Branch / commit:** `claude/landscape-3-0-folge-path-layer-prozeduraler-schnee-sand-mate` @ ff07d825 (code) + doc commit after it, pushed: yes
- **Done:** a landscape has 12 paintable layers; a terrain without a used third page uploads the same bytes and renders the same
  as before; scenes saved before load unchanged; layers 8..11 sample correctly on Metal and OpenGL; MCP/sync/serializer know page 3.
- **Files touched:** `TerrainComponent.h:18-26,89-100` (constants, `layerWeights3`), `MaterialGraph.h:285-291`,
  `MaterialGraph.cpp:1453-1456,1486-1530` (N-page read, `kChan`), `TerrainPaint.cpp` (`paint`, `fillLayer`, `texelWeights`,
  `weightPageCount`, `buildWeightTexture`), `TerrainSystem.cpp:95-139`, `SceneSerializer.cpp:~735-755,~1760-1775`,
  `TerrainSync.cpp/.h` (field 4), `CollabSession.h` (v18), `McpToolsTerrain.cpp:758-885`, `EditorApplication.cpp:7176-7265`
  (`HE_DUMP_LANDSCAPELAYERS=12`), four `M_AutoLandscape*.hasset`.
- **Verified:** `ctest -j4` full suite on the Mac, release build: 345/345. New cases: `test_terrain` (8: paint, fill, fill with
  group shares, byte layout 1/2/3 pages, scene round trip + old scene, system upload), `test_material_graph` (shader text for 9/12
  layers, 5/8/9/12-layer cross-compile for MSL, GLSL 4.10/ES3.00/4.30, HLSL incl. clustered, SPIR-V), `test_mcp_tools_terrain`,
  `test_terrain_sync`. Metal and OpenGL witness (`scripts/he_shot.py ... LANDSCAPELAYERS=12 CAMY=420 PITCH=-90`): lime, pink,
  brown, navy discs at the painted spots, GL and Metal within 3/255. 2:1 regression: the `=8` witness of the pre-change editor (sibling
  worktree deploy) vs this build differs by 228 px (max 3/255) inside the terrain, below the 399 px of two identical runs.
  Help audit 1244/1244.
- **Not verified:** D3D11/D3D12/Vulkan/OpenGL on Windows hardware. No backend file was edited; the text goes through the same
  cross-compilers (tested above on the Mac), binding 14 stays one texture (read: `D3D11MaterialBindings.h`, `D3D12MaterialRootSignature.h`
  `kSlotLandscapeWeights`, `VulkanMaterialLayout.h` row 14, Metal per-draw resolve; none stores a width). The new 3:1 WARP case in
  `test_material_graph.cpp` (D3D11, `_WIN32` only, after "t14 unbound") was written, **not compiled or run** here. CI run
  https://github.com/Horizon-Creations/HorizonEngine/actions/runs/40868603408 was still running when this was written.
- **Decisions taken:** page count N = width / height in the shader (not a fixed 2:1 test), so all four materials were regenerated;
  shares 6/7 and the group logic untouched; `layerWeights3` only beside a second page; `TerrainSync::Weights3 = 4` + protocol 17 -> 18
  (the doc's own rule: an older peer would drop the field); `HE_SDK_RELEASE_SET=OFF` in this worktree's local configure (the
  release-SDK stamp check failed with 'dev MISMATCH' and blocks `HorizonEditor`; unrelated to the change).
- **Decisions suggested, not taken:** cap `weightRes` at 2730 (3:1 stays within 8192 px on small GPUs; today's clamp is 4096 -> 12288 px wide).
- **Pending / found:** the GI bounce kernels (Metal `giLandscapeAlbedo`, GL `uLandWeights`, HLSL `uLandW0..3`, Vulkan) sample the weightmap
  at the full terrain UV and read layers 0..3 only, i.e. they have never known about pages. On a 2:1 map they already read both pages
  squeezed into U; on a 3:1 map the same. Needs `u / N` + a half-texel clamp in four kernels (not done: not this step, not verifiable
  on D3D/Vulkan here). Windows: run the new D3D11 WARP case; Vulkan `maxImageDimension2D` for a 12288 px map.
- **Traps:** the 4 landscape `.hasset` carry the baked GLSL: any edit of `emitLayerBlendNode` needs
  `landscape_tex_gen EditorDeps/EngineContent/Materials --material [--group Forest|Grassland|Desert]` and a matching editor; never copy
  them into another worktree's deploy. `test_terrain_tools_ui.cpp` had `uint8_t[8]` buffers for `texelWeights` (now 12 bytes out).
  First editor build in a fresh worktree: configure with `-DHE_SDK_RELEASE_SET=OFF` unless you want the second release engine tree.
- **Next steps:** 1. step 2 (procedural snow/sand as Material Functions) can use layers 8..11 now; 2. step 3/4 (path / water layer) paint
  layers 8+ via `TerrainPaint::paint`; the foliage "Paint Layer" combo and `terrain_paint` already take 0..11; 3. fix the GI kernels
  (above) before a path layer lands on the third page; 4. read the CI result of run 40868603408 and the Windows D3D11 case.
