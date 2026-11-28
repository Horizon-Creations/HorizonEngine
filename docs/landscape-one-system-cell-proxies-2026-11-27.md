# One landscape system, step 6: mesh HLOD per streaming cell, baked in the editor (topic 355)

## Handoff

**Done** (branch `claude/landscape-one-system`):

- **The bake in steps** (`src/HE_Scene/src/LandscapeCellHlodBake.cpp`, `include/HorizonScene/LandscapeCellHlodBake.h`): `bakeCellProxies` is `cellStatics`
  (parsed cell → static mesh placements, scratch world, main thread) → `resolveCellStatics` (ContentManager: triangles, colour, palette; main thread; a mesh is
  read once per cell, a colour once per mesh+material) → `mergeCellInstances` (transform, rank, budgets, merge; any thread). Same output as before; the export still calls it.
- **`CellHlodBakeJob`** (same files): file read + parse (`parseSceneBytes`) and merge on the job pool, the content work in `pump()` under a ms budget. Only cells whose
  file stamp (size + mtime, `cellFileStamper`) differs from `CellHlodCache::stamps` are baked; vanished cells are dropped; other budgets (`cellHlodOptionsKey`) bake all
  again. The palette is append-only across runs, so old uvs stay valid. A cancelled bake keeps what it finished. `CellHlodCache` (`LandscapeCellHlod.h/.cpp`, 'HLDC' v1)
  is table + stamps + options key; the pack's HLOD chunk is unchanged.
- **Editor module** `src/HE_Editor/CellProxyBake.{h,cpp}`: the shared `EditorConfig::LandscapeFarBake` setting. Off: no bake, no cache, Play draws only a pack's table.
  Manual: **Bake Cell Proxies** (Streaming tab, `StreamingDebugView.cpp`, help key `Profiler/Bake Cell Proxies`). Automatic: a stat pass over the cell files every 10 s
  while idle, a changed cell bakes again by itself. Cache `<project>/Saved/LandscapeHlod/<cell dir>.hlod`, read and written on the pool.
  `tick` sits next to `LandscapeFarBake::tick` in `EditorApplication.cpp`; `EditorApplication::syncCellProxies()` draws the proxies (`m_hlodProxies`) while Play streams.
- `LandscapeHlodProxies::sync`: profiler scope `LandscapeHlodSync`, the table pass iterates in place (`LandscapeHlodTable::forEachCell`, draw radius first), reused
  `m_wanted`, no per-pass vector. `parseSceneBytes` (JSON text or CBOR) is shared with `CellStreamer`; `LandscapeExport` uses the shared `cellFileReader`.
- **Bug found and fixed on the way**: the export's bake read cells with `loadFromMemory`, CBOR only; the loose JSON cells of the editor's splitter counted as unreadable
  and baked nothing. Test: "a loose JSON cell file … bakes like a CBOR one".

**Verified (run in front of me)**

- `HorizonEditor` and `he_tests` build (CLT toolchain). ctest `hlod|streaming_view|help|cell|landscape_far|scene_json|stream`: 19/19; `editor_help_audit.py --check` 1300/1300.
  Full ctest (`-j4`, Release, 357 s): 410/410.
- Tests: `test_landscape_cell_hlod_bake` (loose JSON cell; one edited cell file → `reused 2, baked 1`, palette prefix unchanged, other cell's proxy untouched; options change
  → all; vanished cell dropped; unreadable/missing cell counted once and not retried; cancel; cache round trip) and `test_landscape_hlod` (camera walks past the load radius →
  real meshes 0, proxy entities 2, cells loaded 0).
- **Witness, Metal, `HE_HIDDEN_WINDOW=1`**, `HE_DUMP_PLAYCELLS=1 HE_DUMP_PLAYCELLS_HLOD=1` (13×13 cubes, 16 cells of 40 m, load radius 30 m); pictures on the topic:
  - near (`CAMY=90 PITCH=-89`): "16 proxies baked … play holds **4 of 16 cells** (their real cubes) and draws **12 proxy entities** for the rest (16 made, 4 dropped)": the full
    grid; control without `…_HLOD`: only the 64 real cubes of the 4 built cells;
  - far (`HE_DUMP_PLAYCELLS_CAM=200,40,0,-8,-90`, same editor camera): "play holds **0 of 16 cells** … draws **16 proxy entities**", "0 cubes in the play world": only proxies on screen.
  - bake: 169 cubes → 2028 triangles, 150 KB, 0.4 to 0.6 s on the pool; all of the default cube mesh instances resolved (169 × 12 = 2028, nothing missing).
- **Off / Manual / Automatic** (`HE_DUMP_PLAYCELLS_HLOD=2`, the real module, one cell file touched in each; log attached): Off: button refused, table not drawn, the touched cell stays
  dirty. Manual: "1 cell(s) baked … 15 kept"; pressed again untouched "0 cell(s) baked … 16 kept". Automatic: a touched cell baked by itself 0.6 s later, a second one touched 3 s
  later was baked 7 s after that (the 10 s poll), each "1 baked, 15 kept".

**Not verified**

- Frame cost of the bake in a live editor (2 ms/frame budget, the pool jobs): no hitch measurement. `sync()` allocation change: **not measured** (no `he_perf_capture` run).
- Other backends than Metal for the picture (the proxies are plain meshes with the default material plus the palette, no new shader); Windows.
- Proxies in a project with a real tile landscape and Auto-LODs; the witness world is cubes. A mesh with several sections uses the first section's material colour.
- The export does not reuse the editor cache (it bakes all cells itself).

**Decisions**: JobSystem pool jobs (short, no waits) instead of a `std::thread` like `TileJob`: nothing here waits for the main thread. ContentManager is main-thread only,
hence the three steps. Dirty key = size + mtime, not a content hash (a touch re-bakes one cell; a changed mesh or material asset is not seen, split again). Proxies show in
Play only: edit mode has no streamer and holds every cell's content. One shared setting, as the topic says; its Preferences text names the cell proxies.

**Pitfalls**

- Build on this Mac: run builds with `DEVELOPER_DIR=/Library/Developer/CommandLineTools`; after a `CMakeLists.txt` edit the reconfigure otherwise fails in assimp ("tried
  including <stdio.h>", cached `ZLIB_INCLUDE_DIR` is the CLT SDK, `xcode-select` points at Xcode).
- Witness: `HE_DUMP_PLAYCELLS_CAM` yaw: with the camera at +x, yaw −90 looks at the grid, +90 away from it. The log line of the Off result can be cut by another thread's
  line (interleaving), the next lines say the rest.
- A Bake press during the cache write of the previous bake is ignored (milliseconds).

**Next steps**

1. Let the export use `Saved/LandscapeHlod` like the far image uses `Saved/LandscapeFar` (`LandscapeExportOptions`).
2. Watch a real project with trees and buildings (`he_flythrough.py`): proxy colour against the real meshes, the pop where a cell builds.
3. Optional: instance decimation (Auto-LOD's coarsest level) in `mergeCellInstances`; today only the budgets cut instances.
