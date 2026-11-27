# One landscape system, step 6: mesh HLOD per streaming cell, baked in the editor (topic 355)

Status: **partial** (the queen asked to stop). The core bake and its tests are verified; the editor wiring is written and **not compiled**.

## Handoff

**Done and verified** (commit "Cell proxies: the bake in steps…"):

- `src/HE_Scene/src/LandscapeCellHlodBake.cpp` / `include/HorizonScene/LandscapeCellHlodBake.h`: `bakeCellProxies` is now three steps that the editor can spread over threads:
  `cellStatics` (parsed cell → static mesh placements; builds a scratch world, main thread) → `resolveCellStatics` (ContentManager: mesh triangles, colour, palette; main thread; a mesh is read once per cell, a colour once per mesh+material) → `mergeCellInstances` (transform, rank, budget, merge; any thread).
  Output is the same as before (the old tests pass unchanged).
- **`CellHlodBakeJob`** (same files): reads+parses cell files on the job pool (`parseSceneBytes`), resolves content in `pump()` under a ms budget, merges on the pool. Only cells whose file stamp (size + mtime, `cellFileStamper`) differs from `CellHlodCache::stamps` are baked; vanished cells are dropped; other budgets (`cellHlodOptionsKey`) re-bake all. The palette is append-only across runs, so old uvs stay valid. A cancelled bake keeps what it finished.
- `CellHlodCache` (`LandscapeCellHlod.h/.cpp`): table + stamps + options key, `encode`/`decode` ('HLDC' v1). Not a pack chunk: the pack's HLOD section is unchanged.
- `SceneJsonParse`: `parseSceneBytes` (JSON text or CBOR by first byte), now also used by `CellStreamer`.
- **Latent bug found and fixed on the way**: the export's bake used `SceneSerializer::loadFromMemory`, which reads CBOR only. Loose JSON cell files (what the editor's splitter writes) counted as "unreadable". Test: "a loose JSON cell file … bakes like a CBOR one".
- `LandscapeHlodProxies::sync`: profiler scope `LandscapeHlodSync`; the table pass iterates in place (`LandscapeHlodTable::forEachCell`) with the draw radius first, a reused `m_wanted` set, no `cells()`/`hlodCellsToDraw` vector per pass. **Not measured.**
- Tests (16 cases, 272 assertions green, built as a standalone binary from `test_landscape_cell_hlod_bake.cpp` + `test_landscape_hlod.cpp` against the build tree's libs; script `/tmp/hlod_mini_tests.sh` pattern: flags and link line from `cmake-build-release/tests/CMakeFiles/he_tests.dir/{flags.make,link.txt}`):
  edit one cell file → `reused == 2, baked == 1`, palette prefix unchanged; cancel; unreadable/missing cell counted once; cache round trip; camera walks past the load radius → real meshes 0, proxies 2, loaded 0.
- `python3 scripts/editor_help_audit.py --check`: 1300/1300.

**Written, NOT compiled, NOT run** (separate commit "Editor: …"):

- `src/HE_Editor/CellProxyBake.{h,cpp}`: the editor module. `tick` (per frame while editing; next to `LandscapeFarBake::tick` in `EditorApplication.cpp`), `bakeNow` (button), `table(mode)`, `summary`, `setWitnessRoot`. Setting is the shared `EditorConfig::LandscapeFarBake`: Off = no bake, no cache, Play draws only a pack's table; Manual = button **Bake Cell Proxies** (Streaming tab, `StreamingDebugView.cpp`, help key `Profiler/Bake Cell Proxies`); Automatic = a stat pass over the cell files every 10 s while idle, changed cells re-bake. Cache: `<project>/Saved/LandscapeHlod/<cell dir>.hlod`, read/written on the pool.
- `EditorApplication::syncCellProxies()` (after the Landscape block): `m_hlodProxies.sync` only while Play streams (`m_playCells->runtime().active()`), table = editor cache, else the pack's; `clear` otherwise.
- Witness: `HE_DUMP_PLAYCELLS=1 HE_DUMP_PLAYCELLS_HLOD=1` (+ `HE_DUMP_PLAYCELLS_CAM=x,y,z[,pitch,yaw]` for the far shot) bakes the witness cells through the module, plays, logs `HE_DUMP_PLAYCELLS_HLOD … proxies baked … play holds N of M cells … draws K proxy entities`. Cubes are counted without proxies.
- `tests/CMakeLists.txt` adds `CellProxyBake.cpp` next to `StreamingDebugView.cpp` (it calls the module). The Preferences text (`EditorSettingsCatalog.cpp`, help key `Preferences/Content Browser/Far Landscape Bake`) now names the cell proxies too.

**Not done / not verified**

- `HorizonEditor` and the full `he_tests` were not built (the queen stopped the run during the editor build; no compile error had shown yet). Expect small compile fixes in `CellProxyBake.cpp` / `EditorApplication.cpp`.
- Near/far witness pictures: not taken. The play camera in that witness may be a world camera entity, not the editor camera; `HE_DUMP_PLAYCELLS_CAM` moves (or creates) it. Whether the default cube mesh (`kDefaultCubeMeshId`) resolves in `resolveCellStatics` is unchecked (the log's `meshesMissing`).
- Off/Manual/Automatic behaviour in the real editor; Windows; other backends; no perf run.
- The export still bakes everything itself; it does not reuse the editor's cache (`LandscapeExport.cpp` only uses the shared `cellFileReader` now).

**Decisions**: JobSystem (short pool jobs, no waits) instead of a `std::thread` like `TileJob`, because nothing in this bake waits for the main thread. Dirty key = file size+mtime, not a content hash (a touch re-bakes one cell; a changed mesh/material asset is not seen). Proxies show in Play only: edit mode has no streamer and holds every cell's content.

**Build trap (this Mac)**: after editing a `CMakeLists.txt` the next `cmake --build` reconfigures and the assimp dependency fails with "tried including <stdio.h>" (the cached `ZLIB_INCLUDE_DIR` is the CLT SDK while `xcode-select` points at Xcode). Run builds with `DEVELOPER_DIR=/Library/Developer/CommandLineTools`. A header edit in HE_Scene costs ~15 min of rebuild (HorizonScene + HorizonCore + SDL); the full `he_tests` has ~420 objects.

**Next steps**
1. `DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build cmake-build-release --target HorizonEditor -j8`, fix compile errors; then `he_tests`, `ctest -R "hlod|streaming_view|help"`.
2. Run the witness twice (Metal, `HE_HIDDEN_WINDOW=1`, `scripts/he_shot.py` style): near `HE_DUMP_PLAYCELLS=1 HE_DUMP_PLAYCELLS_HLOD=1 CAMY=90 PITCH=-89`, far `… HE_DUMP_PLAYCELLS_CAM=400,40,0,-8,90`; read the `HE_DUMP_PLAYCELLS_HLOD` log lines for the proxy / loaded-cell counts; attach both pictures.
3. Try Manual and Automatic in the editor on a split scene (edit one cell file, watch the summary line say 1 baked).
4. Optional: let the export reuse `Saved/LandscapeHlod` (`LandscapeExportOptions`), as the far image reuses `Saved/LandscapeFar`.
