# Landscape world scale, step 6: region gate for entity cells, HLOD data model

Thema 326, Hive step 6 ("Entladen ganzer Regionen, Entity-Zellen und HLOD-Backen"). Plan:
`docs/landscape-world-scale-plan-2026-10-28.md`. Branch `claude/landscape-weltmassstab-100km`.

## What the step asked and what was already there

The step's own wording: regions (~8 km) with hysteresis (load R, unload 1.25 R) and a memory
budget; on unload, tile textures, Jolt shapes, nav tiles, foliage buffers, water bodies and
entity cells go together, the coarse mesh stays; `CellStreamer` extended (not replaced) for
the region level; stateful entities per `docs/entity-cell-streaming-plan-2026-10-10.md` D3;
HLOD proxy meshes per region for props/buildings, baked at export, stored in the pak.

Two surveys first (see Handoff below for how they were checked), both cutting the actual
scope a lot:

1. **`LandscapeStoreConfig::loadRadius` is already 8000 m with `keepFactor` 1.25** —
   `src/HE_Scene/include/HorizonScene/LandscapeStore.h:46-47`. Tile textures already unload at
   region scale. Physics (1.5 km), foliage and navigation (their own, smaller radii) and water
   (an n×n tile window) all depend on a tile being resident in the store, so they already lose
   their own per-tile state when the store evicts a tile — `tests/test_landscape_store.cpp`
   ("walking 100 km keeps the resident set inside the kept ring, under the budget, without
   duplicates") already witnesses this for the store; nothing here needed to add an
   `unloadRegion()` to each subsystem, which would have fought their own hysteresis for no
   reason (a forced release racing a subsystem's own eviction is a thrash bug waiting to
   happen, not a feature).
2. **D3 (stateful entities) already exists**: `HE::CellState` + `HE::CellRuntime`
   (`src/HE_Scene/include/HorizonScene/CellState.h`, `CellRuntime.h`), built in Thema 164 steps
   2c/3b. Tombstones and `SaveState` already ride `CellStreamer`'s own `loaded`/`unloading`
   hooks. A region-driven cell unload goes through the exact same hooks as any other cell
   unload, so D3 did not need new code here — only a test that it still holds when the
   *reason* for the unload is the region gate rather than the cell's own radius (the "no
   thrash" test below exercises the same hook path a stateful unload would).

What was actually missing, and what this step built:

- Entity cells (`CellStreamer`) have their own manifest radii, completely independent of the
  landscape's region scale — nothing tied a cell's unload to "its region fell out of range".
- Nothing represented an HLOD proxy for cell content at all.

## Built

### 1. `CellStreamer`'s region gate (extends, does not replace)

`src/HE_Scene/include/HorizonScene/CellStreamer.h` / `.cpp`:

- `CellManifest::regionOf(cellX, cellZ, cellSize, regionSize)` — pure arithmetic,
  `floor(cellX * cellSize / regionSize)`; `regionSize <= 0` returns the cell coordinates
  unchanged (the gate's "off" value matches "no grouping").
- `CellRegionConfig { regionSize = 0; loadRadius = 8000; keepFactor = 1.25; }`,
  `CellStreamer::setRegionConfig`/`regionConfig()`/`isRegionResident(x, z)`.
- `CellStreamer::update()` gained one new block (`CellStreamer.cpp:453-501`, marked `// R)`),
  run once per update before the existing cell scan: the same per-anchor hysteresis the cells
  themselves use (`nearestReach`/`squareDistance`, reused as-is), one level coarser. A region
  becomes resident within `loadRadius` of an anchor, stays until every anchor is beyond
  `loadRadius * keepFactor`, and fires `Hooks::regionLoaded`/`regionUnloaded` on the
  transition.
- Three call sites changed, each by one extra condition, guarded by `regionResident(x, z)`
  (which is `true` unconditionally when `regionSize <= 0`, so every existing test and every
  manifest without regions behaves exactly as before — verified by the untouched
  `test_cell_split.cpp`/`test_cell_runtime.cpp`/`test_cell_village.cpp`/`test_cell_state.cpp`
  suites, 34/34 passing unchanged):
  - the "wanted" scan (step 1) skips a cell whose region is not resident — a region gates
    cells IN, on top of the cell's own radius;
  - the pending-cell cancel check (step 3) also cancels/abandons a cell whose region just
    stopped being resident, so nothing keeps building into a region the gate already dropped;
  - the unload scan (step 5) force-unloads a loaded cell whose region is not resident, even
    when the cell's OWN manifest radius would have kept it — this is the actual "region
    unloads its cells together" behaviour the step asked for.
- `Stats::regionsResident`.

**Why this shape:** the draw/build unit stays the cell (unchanged file format, unchanged
slice/hook contract); the region is purely a second, coarser admission test layered on the
existing scan/unload passes. `CellManifest`'s own per-cell radii keep meaning what they always
meant (fine-grained, e.g. "within this many metres, build me"); `CellRegionConfig` is a
separate, optional policy a caller (a landscape runtime, a game) sets once it decides regions
matter, independent of any one manifest's numbers.

**Tests** (`tests/test_landscape_region_streaming.cpp`, 4 cases, numbers chosen so the
region's own centre-to-edge distance never ties with the test radii — see the file's
comments for the arithmetic):
1. a cell only loads while its region is resident (an out-of-reach region's cell never even
   starts reading);
2. wobbling across the region's load/keep band (not its own edge) does not thrash — one
   `regionLoaded`, zero `regionUnloaded`, zero `unloads` across 12 oscillations;
3. a region unload force-drops cells the manifest's OWN (much larger) radius would have kept
   — the actual behaviour change;
4. `regionSize == 0` (the default): `isRegionResident` is always false, `regionsResident` stat
   stays 0, cells load exactly as every pre-existing test already shows.

### 2. `LandscapeHlod` — the proxy data model and CPU selector (new)

`src/HE_Scene/include/HorizonScene/LandscapeHlod.h` / `src/HE_Scene/src/LandscapeHlod.cpp`,
`tests/test_landscape_hlod.cpp` (5 cases). Deliberately data-and-selector only; see "Not done"
below for the baker source, the pak format and the drawing.

- `LandscapeHlodVertex` (pos/normal/uv), `LandscapeHlodMesh` (vertices + indices).
- `LandscapeHlodSourceMesh` + `bakeCellHlod(sources)`: merges triangle soups into one mesh.
  V1 is concatenation only — no shared-vertex welding, no decimation, no impostors (trees are
  procedural per-tile foliage, not cell entities, and need a billboard atlas, not this path).
  A source without normals gets the flat per-triangle normal; a malformed source (index count
  not a multiple of 3, or an out-of-range index) is skipped rather than corrupting the merge.
- `LandscapeHlodTable`: `set`/`find`/`has`/`cells()` by `(cellX, cellZ)` — the SAME keys
  `CellManifest`'s cells use, on purpose (see "why per cell" below) — plus `encode()`/
  `decode()`: a small little-endian binary format (`'HLDT'`, version, cell count, then each
  cell's key and raw vertex/index arrays) meant to become one pak chunk per region later
  (nothing stops one `LandscapeHlodTable` per region today; a pak writer would just encode one
  per region instead of one for everything).
- `hlodCellsToDraw(table, streamer)`: the CPU-only selector. A cell is selected iff the table
  has it AND `CellStreamer::isLoaded` is false for it — deliberately not consulting region
  residency, because by the time a region is not resident its cells are not loaded either (the
  gate above), so asking twice would answer the same question. `isBuilding` is also not
  checked: a half-built cell should KEEP its proxy, which the selector gets for free by only
  keying off `isLoaded`; the intended entity wiring (below) hides a proxy on
  `CellStreamer::Hooks::loaded`, not `loadedSlice`, and shows it again on `unloading`, so a
  proxy is visible for exactly the frames its cell has no complete entities yet.

**Why per cell, not per region:** standing inside a resident ~8 km region you are usually near
SOME of its cells (a few hundred metres each) and far from others; a single region-wide proxy
mesh would either double-draw next to a loaded cell or leave a hole where a far cell of the
same resident region has no proxy. The region only ever decided which cells COULD need a
proxy at all (`isRegionResident`); which ones actually draw is a per-cell question, answered
by `hlodCellsToDraw`.

## Verified

- `he_tests`, Release, x64, scratch `%APPDATA%` (he_tests wipes the real one — see memory
  `vulkan-d3d-baseline-noise`): `test_landscape_region_streaming.cpp` (4/4),
  `test_landscape_hlod.cpp` (5/5), and the full landscape + cell suite together
  (`test_cell_*`, `test_landscape_*`, `test_world_scale.cpp`): **214/214 test cases, 0
  failed**. A full `he_tests` run (all ~6000+ cases, not just landscape/cell) was started
  before this doc's Handoff was written; see the Handoff for its result.
- Built `he_tests` only (`cmake --build out\build\x64-release --target he_tests -j 8`,
  MSVC via `vcvars64.bat`); `HorizonEditor`/`HorizonGame`/`landscape_gpu_check` were NOT
  rebuilt or exercised in this step — nothing here touches a renderer, and the step's own
  "Done when" items 4/5 (HLOD rendering, export baking) are explicitly not built yet (below).

## Not done (the step asked for more than this pass covers)

- **The bake source.** `bakeCellHlod` takes already-resolved triangles; nothing resolves a
  cell's placed `StaticMeshComponent`/prefab-building assets into them. That is the export
  pipeline's job (`src/HE_Editor/ExportDialogPanel.cpp`), not started.
- **Pak storage.** `LandscapeHlodTable::encode/decode` exist and round-trip, but nothing
  extends `LandscapeSource.h`'s pack format (`LandscapePackWriter`/`LandscapePackSource`) to
  carry an HLOD chunk, versioned so v1/v2 paks keep loading.
- **Drawing.** No `LandscapeHlodProxies` class exists; `LandscapeWorldProxies.h` (hidden
  entity + `MeshComponent`, the same pattern foliage/water already use on every backend with
  zero backend code) is the intended shape, named in `LandscapeHlod.h`'s file comment, not
  built.
- **Tree impostors.** Explicitly out of scope for cell HLOD (trees are procedural foliage).
- **Editor UI**, a baking button/panel, `editor_help_audit.py` entries: none needed yet since
  nothing UI-facing was added.
- **GPU/perf acceptance** (1% low < 8 ms, no hitch > 16 ms, RAM budget over a real 100 km
  flight on the RTX 4070): not measured. Everything verified above is CPU-only, in
  `he_tests`, with tiny synthetic manifests (2 cells) chosen to make the region arithmetic
  checkable by hand — not a measurement of the real cost at landscape scale.

## Handoff

- **Topic / step:** 326, Hive step 6 of the plan ("Entladen ganzer Regionen, Entity-Zellen und
  HLOD-Backen").
- **Branch / commit:** `claude/landscape-weltmassstab-100km` — see the Hive post for the
  commit hash this doc was pushed with.
- **Done:** `CellStreamer` has a region-level admission/eviction gate on top of its existing
  per-cell hysteresis (off by default, `CellRegionConfig::regionSize <= 0`), and a new
  `HE::LandscapeHlod*` data model (merge/bake, pak-ready binary encode/decode, a CPU selector
  for which cell's proxy should draw).
- **Files touched:** `src/HE_Scene/include/HorizonScene/CellStreamer.h`,
  `src/HE_Scene/src/CellStreamer.cpp` (region gate), `src/HE_Scene/include/HorizonScene/
  LandscapeHlod.h`, `src/HE_Scene/src/LandscapeHlod.cpp` (new), `src/HE_Scene/CMakeLists.txt`,
  `tests/test_landscape_region_streaming.cpp` (new), `tests/test_landscape_hlod.cpp` (new),
  `tests/CMakeLists.txt`.
- **Verified:** see "Verified" above — 214/214 on the landscape+cell suite; a full
  `he_tests` run's result goes in the Hive post, not here (it was still running when this
  doc was written).
- **Not verified:** everything under "Not done" above; also not re-run on Linux/macOS/CI —
  only built and run on NN-WS03 (Windows, MSVC, Release).
- **Decisions taken:** region gate extends `CellStreamer` in place rather than a separate
  `LandscapeRegionStreamer` class (the step's own text offered both; extending kept one
  hysteresis implementation instead of two, and needed no new coupling between classes) —
  reason in "Why this shape" above. HLOD keyed per cell, not per region — reason in "Why per
  cell, not per region" above. No `unloadRegion()` added to `LandscapePhysics`/`LandscapeFoliage`/
  `LandscapeNavigation`/`LandscapeWater` — reason in survey #1 above.
- **Decisions suggested, not taken:** whether `CellRegionConfig` should be read from the
  scene's `cells` manifest JSON (a `regionSize` key next to `cellSize`) instead of being set
  by application code at runtime — left to whoever wires a landscape runtime to a
  `CellRuntime`/`GameApplication`, since that is also where the actual region radius (probably
  the landscape's own 8000 m `loadRadius`) should come from, not a cell-streaming concern.
- **Pending:** the export baker (resolve cell meshes → `LandscapeHlodSourceMesh`), the pak
  format's HLOD chunk, `LandscapeHlodProxies` (the drawing side, mirroring
  `LandscapeWorldProxies`), wiring `CellRuntime`/`GameApplication`/`EditorApplication` to set a
  real `CellRegionConfig` from the landscape's own region size, tree impostors, and the whole
  GPU/perf acceptance pass (100 km flight, 1% lows, hitches, RAM budget) — none started.
- **Traps:** region-size/radius values that tie exactly at a region's own half-width (e.g.
  `loadRadius == regionSize / 2`) make the anchor's own region AND its neighbours all resident
  at once (the `squareDistance` clamp formula gives exactly `0` for a point ON a shared
  boundary) — pick a load radius strictly under that, or expect extra regions in
  `regionsResident`/`isRegionResident`. `CellStreamer::m_index[k]` style lookups used
  throughout this file are non-const `operator[]`; keep new code inside `update()` (non-const)
  or use `.find()`/`.at()` elsewhere.
- **Next steps:** 1. Wire a real `CellRegionConfig` from whatever already knows the
  landscape's region/load radius (`LandscapeRuntime`/`LandscapeRuntimeConfig`,
  `src/HE_Scene/include/HorizonScene/LandscapeRuntime.h`) into `CellRuntime`/`CellStreamer` at
  the application level. 2. Build the export-side baker that turns a cell's placed static
  meshes into `LandscapeHlodSourceMesh` and calls `bakeCellHlod`, in
  `src/HE_Editor/ExportDialogPanel.cpp`. 3. Extend `LandscapeSource.h`'s pack format with a
  versioned HLOD chunk (one `LandscapeHlodTable::encode()` blob per region, offset+size in the
  index, like the water table). 4. Build `LandscapeHlodProxies` mirroring
  `LandscapeWorldProxies.h`, driven by `hlodCellsToDraw` and the `loaded`/`unloading` hooks.
  5. Only once 1-4 exist: the GPU/perf acceptance flight.
