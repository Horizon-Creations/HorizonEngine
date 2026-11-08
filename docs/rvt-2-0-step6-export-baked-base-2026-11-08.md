# RVT 2.0 step 6: the export bakes the base (Thema 335, 2026-11-08)

Part (a) done, part (b) blocked.

## What it does
`HorizonScene/RvtBaseBake.{h,cpp}`:
* **Export** (`ExportDialogPanel.cpp`, sibling of the far-landscape bake; checkbox "Bake RVT base" + budget, profile fields `bakeRvtBase` / `rvtBaseBudgetMB`, default on / 256 MB): for every square, unrotated landscape whose material has an RVT Write/Read pair, `HlodBaker::begin(rvtPages=true)` + `bakeRect` in 1024 px tiles photographs the three page layers (colour, normal, AO+roughness = the Write pins, no snow/sand/puddles/trails) over the whole landscape into ONE texture per layer. Side = live page resolution (0.25 m/texel) rounded up to a power of two, capped at 4096, halved until the cooked cost (side² x 4 B) fits the budget. Packed as `ExtraAsset`s (single-level RGBA8, so the packer cooks BC7/ASTC), under `Baked/Landscape/` (already a reach root).
* **Key = the gate.** Ids are `rvtBaseAssetId(terrainId, key, kind)`. `key` = `matRvtMaterialState` of the page subgraphs (params/switches/live nodes; env built by the shared `rvtMaterialEnv`, also used by `RvtLandscape::refreshAnalysis`) + size/place + a hash of the ground (seed/noise params, `sculptHeights`, `layerWeights*`). No asset revisions in it (pak pixels differ from the editor's). A param after the Read node does not move it; a param before the Write node, a vertex or a paint texel does.
* **Runtime** (`RvtLandscape::attach`): if `rvtBaseViewMaterials` finds the three textures for the landscape's current key, `HlodBaker::begin(..., bakedViews)` uses three texture-sampling unlit views (world pos -> uv -> TextureSample) instead of building the page graphs; the page painter, pool, table, consumer (live part after the Read) are unchanged, so it works on every backend, no new `IRenderer` call.
* **Leaving the baked base, for good (`seg.bakedOff`):** a journal edit the pages read (checked for every attached baked segment, in range or not) or a change of a page input (stateHash) -> `remove` + re-attach from the page graphs. A baked baker refuses `rebuildRvtViews(true)`. Live-only changes (snow line ...) keep the baked base.

## Verified (Metal, this Mac, release)
* `he_tests`: new `test_rvt_base_bake.cpp` (7 cases: ids, no key without RVT nodes, key follows page inputs/ground/place but not after-Read params, pictures found by key only and become 1-texture views, bad input refused, side planner) and a pack round trip in `test_reach_filter.cpp` (export -> pak -> `ensureResident` by derived id, other key not found). `*test_rvt_*` 55, `*test_landscape_*` 38, `*test_hlod_*` 7, `*test_reach_filter` 9, `*test_project_exporter` 42, `*test_material_graph` 109: all green. `editor_help_audit.py --check` green.
* Witness `HE_DUMP_AUTOLAND=1 HE_DUMP_RVTBASE=1 HE_RVT=1 HE_DUMP_RHI=Metal HE_HIDDEN_WINDOW=1 HE_DUMP_PATH=x.bmp HE_DUMP_QUIT=1 HorizonEditor` (private HOME, 128 m landscape): reference from graphs, bake 512x512 (63 ms, 3.0 MB raw), pak mount of 3 textures, second driver logs "segment attached ... (pages from the export's baked base, the page graphs are not built)", `1 attached (1 from the baked base)`; pages vs reference mean 0.13/255 (max 42), vs whole material 1.11 (reference 1.07). A ground edit logs "leaves the export's baked base", re-attaches live, matches the material again (1.07). A fresh driver on the exported ground: baked; with one vertex raised: not baked.

## Not verified / limits
* **No real export -> HorizonGame run.** The Export dialog button and the game's pak mount were not driven; the pak step is covered by the reach test, the rest by the witness (loose textures via `loadHlodAssets`). Please run one export of a project with an Auto Landscape and read the log for "RVT base: ..." and, in the game, "(pages from the export's baked base ...)".
* **The key must be equal in editor export and game.** If the game's `TerrainComponent` differs after load in a field I hash (or `shaderParamData` differs), the pictures are silently not used (log says "attached" without the baked note). First thing to check in the real run.
* Texture resolution: pictures are capped at 4096 px, so landscapes over ~1 km are baked coarser than the live pages (blurrier near mip 0; the log says "below the live pages' resolution"). Page-by-page storage (finer near, coarse far) would fix it but needs a bytes->pool-slot upload per backend (Metal cold store `RvtColdWork`, D3D11 `UpdateRvtCold`) which does not exist.
* A script that edits the ground in the SAME frame before the first attach is caught by the key (ground hash), not by the journal (the load notes "all" itself and attach clears it).
* D3D/Vulkan/GL not run (the views are ordinary graphs; the bake needs `supportsRawCapture`).
* Non-square landscapes and landscapes that are not Auto Landscape groups are skipped.

## Part (b): blocked
`LandscapeStore` / `kLandscapeFS` are not on `main` or this branch; only on `origin/claude/landscape-weltmassstab-100km` (files `HorizonScene/LandscapeStore.h`, `src/LandscapeStore.cpp`, `LandscapeStoreBench`; `kLandscapeFS` PR 214). Not copied, per the step's limit. Needs: that branch merged, then volume per tile load/unload, `kRvtLookupGLSL` in `kLandscapeFS`, a scratch world per tile, and the key above per tile (the ground hash would have to be per tile).

## Handoff
Next steps, in order: (1) real export + game run (see above); (2) once PR 214 is in: part (b); (3) page-by-page storage if 4096 px is too coarse (needs the per-backend slot upload); (4) per-layer stale flags (step 4 handoff, untouched).
Traps: `ninja` from `out/build/release` with CLion's ninja; `HlodBaker::bakedViews()` bakers cannot rebuild views; the baked views register 3 materials per attach of a new baker (never freed, like the page views).
