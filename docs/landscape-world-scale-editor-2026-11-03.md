# Landscape world scale, step 6: editor tools (pack builders, Tile Landscape section, streaming view)

Thema 326, Hive step 6 of the plan ("Editor-Werkzeuge: Import, Sculpt/Paint auf Tiles, Migration,
Streaming-Ansicht"). Plan: `docs/landscape-world-scale-plan-2026-10-28.md` §3.8. Branch
`claude/landscape-weltmassstab-100km`.

## The decision the step hung on

The step says its scope depends on "paint 100 km by hand, or from imports and generators". Taken:
**imports and generators**. 100 km at 2 m is 1.9 billion height samples; nobody paints that, and the
format has a reason of its own against editing in place: a pack (`LandscapeSource.h`) appends its
tile blobs back to back behind a fixed index, so a tile whose compressed size changes cannot be
rewritten where it is. Sculpt/paint that writes tiles back needs a writable store (an overlay of
edited tiles over the pack, saved by streaming a new pack, or a loose-tile directory). That is a
format decision, not an afternoon, so it is **not** in this step (see Pending).

## What exists now

| Piece | Where | What |
|---|---|---|
| Pack builders | `HE_Scene/include/HorizonScene/LandscapePackBuild.h`, `src/LandscapePackBuild.cpp` | `buildProceduralPack`, `migrateTerrainToPack`, `importHeightmapToPack` (+ `findHeightmapTileFiles`, `singleHeightmapFile`), `LandscapeBuildJob` (thread + progress + cancel). No ImGui. |
| 16-bit file reader | `TerrainHeightmap::readGrey16` (`TerrainHeightmap.h/.cpp`) | The formats of `importFile` (PNG 8/16, PGM, `.r16/.raw`) as 16-bit grey; shared so the tile import and the single-landscape import read files the same way. |
| Details section | `HE_Editor/LandscapeWorldTools.h/.cpp`, `InspectorPanel.cpp` ("Tile Landscape", Add Component ▸ Rendering) | Component settings (pack, radius, budget, height range, texture group), output file, Generate / Migrate / Import buttons, progress bar, Cancel, status line. |
| Streaming tab | `StreamingDebugView.cpp` `drawLandscape` | Tiles resident/loading/wanted, memory vs budget with peak, update ms, atlas slots, physics/foliage/GI, a 25 x 25 tile map around the editor camera. |
| Tests | `tests/test_landscape_pack_build.cpp`, `tests/test_landscape_world_ui.cpp` | See Verified. |

Behaviour worth knowing:

* **Writes are atomic**: the pack is built as `<path>.part` and renamed over the target at the end. A
  cancelled or failed build leaves the old pack alone and removes the part file.
* **Same path, new pack**: the runtime only reopens a pack when the component's path *changes*. When a
  build overwrites the pack the component already names, `LandscapeWorldTools::pump` switches the
  component off for two frames (the runtime lets go of the old file), then on again. `pump` is called
  from `EditorApplication::OnRender` next to the landscape update, before it.
* **Migration** puts the Terrain's south-west corner at the pack origin, adds the Terrain entity's world
  y, fits the height range (`floor(lowest)` .. span) unless given, de-interleaves the three weight pages
  into the 12 channels, and clamps the last tile row/column at the terrain's edge. A terrain whose cell is
  the tile raster (1025 samples over 2048 m against 2 m) is copied exactly. Water is **not** migrated.
* **Import**: a pixel is a height *sample*, `metersPerPixel` apart (N pixels span (N-1) x mpp). Stored
  height = the picture's 16-bit value (8-bit widened by 257); `heightMin/Range` only say what that means in
  metres and are written to the pack header and the component. "Flip File Rows" reverses the order of the
  files (y0 = southern-most, World Machine), not the rows inside a picture. Files are read when a sample
  needs them (12 kept, least recently used out), so a set far larger than memory imports. A missing file
  is a hole (height 0); a file of another size, or one that will not read, fails the import and removes
  the pack. "Paint from shape" writes flat / steep / high into layers 0 / 1 / 2.
* **Absolute paths**: the section writes the component's `pack` as an absolute path because
  `LandscapeRuntime::beginFromPack` opens the string as it is (relative to the process's working
  directory, not the project), in the editor *and* in `GameApplication`. The component's comment still
  says "project-relative"; that is not implemented anywhere (see Pending).

## Verified

* `he_tests` built with ninja `-j8` on the Mac (Apple clang, Release); see the Hive post for the run of
  the new and the neighbouring suites. Test names: `pack build: ...` (7 cases) in
  `test_landscape_pack_build.cpp`; `landscape world tools: ...` (4) and `streaming view: a running tile
  landscape ...` in `test_landscape_world_ui.cpp`.
* The migration round-trip is the check that "the landscape is in the tile store": terrain -> pack ->
  `LandscapePackSource` -> `LandscapeStore` resident tile -> `landscapeTileTerrain` equals the original
  heights within half a quantum (`scale / 65535`) and the weights byte for byte.
* `python3 scripts/editor_help_audit.py --check` passes with the 28 new `Tile Landscape/...` entries.

## Not verified

* A 100 km generate (196 x 196 tiles) was not run: it is minutes and several GB; the tests build 4 x 4.
  `landscape_store_bench` has the measured per-tile cost.
* The native file dialogs (`SDL_Show*Dialog`) and the live editor: only the widgets are drawn headless.
* Windows/Linux CI; the code uses `std::filesystem`, `std::thread`, `std::async` only.

## Pending (in order)

1. **Sculpt/paint on tiles that write back.** Needs the writable-store decision above. The reusable
   piece exists: `landscapeRegionTerrain` turns an n x n block of resident tiles (n <= 4, 2 km) into one
   `TerrainComponent`, so the existing Landscape tools could edit a "region" proxy; the missing half is
   the inverse (terrain -> tiles, `buildMips`) and a pack save that streams unchanged tiles through (pass
   the old coarse samples to `LandscapePackWriter::add` so 38 000 tiles are not decoded again).
2. Project-relative pack paths (resolve against the project root in `LandscapeRuntime::update`, which
   needs the root passed in; `LandscapeRuntime.h` is included by `HorizonWorld.h`, so touching it
   rebuilds everything).
3. Export: the pack is a file beside the project, not part of the pak.
4. Water for migrated terrains (`water::Field` -> coverage/owner rasters + a water table).
5. A per-key "failed" state on `LandscapeStore` so the tile map can show it (only the count exists).

## Handoff

- **Topic / step:** 326, Hive step 7 of 10 ("Schritt 6: Editor-Werkzeuge")
- **Branch / commit:** `claude/landscape-weltmassstab-100km` (see the Hive post for the hash; pushed: see post)
- **Done:** Pack builders (procedural, Terrain migration, 16-bit/tiled height map import) with a threaded,
  cancellable job; the "Tile Landscape" Details section with generate/migrate/import; the Streaming tab's
  tile-landscape section; help entries; tests. Sculpt/paint write-back is deliberately not done.
- **Files touched:** `src/HE_Scene/{include/HorizonScene,src}/LandscapePackBuild.*` (new),
  `TerrainHeightmap.{h,cpp}` (`readGrey16`), `src/HE_Editor/LandscapeWorldTools.*` (new),
  `InspectorPanel.cpp` (section + Add Component row), `InspectorPrefabKeys.cpp`, `EditorHelp.cpp`
  (scope `Tile Landscape`, `Component/Tile Landscape`, 28 entries), `StreamingDebugView.cpp`,
  `EditorApplication.cpp` (`pump` call), CMakeLists of HE_Scene/HE_Editor/tests.
- **Decisions taken:** imports and generators instead of hand painting (above); atomic `.part` + rename;
  absolute pack paths; component off/on to reopen a rewritten pack; flipY reverses file order only.
- **Traps:** `LandscapeRuntime` compares the pack *string*; a failed pack is remembered by string until it
  changes. `TerrainHeightmap.h` is included by `TerrainTools.cpp`, `TerrainSystem.cpp` and
  `test_terrain_heightmap.cpp`: touching it recompiles those. Do not put a LandscapeWorld entity and a
  Terrain on the same entity if you also want the Terrain to stay: the runtime's foliage reads the
  FoliageComponent of the landscape entity and the single-landscape system would scatter it too.
- **Next steps:** 1. the writable store / region edit (Pending 1). 2. project-relative paths. 3. step 7
  (verification on all five backends) can use `LandscapeWorldTools::startGenerate` for its 100 km world.
