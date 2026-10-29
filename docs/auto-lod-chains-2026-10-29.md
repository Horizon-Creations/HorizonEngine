# Auto-LOD chains: import and "Generate LODs" (Thema 331, step 3)

**State: WIP, written but not compiled or tested.** The queen stopped the run while the first
`he_tests` build was at about 1300 of 1709 steps. Nothing below has been run.

## What was built

* `src/HE_Tools/src/MeshSimplify/AutoLod.{h,cpp}` (in HorizonImporters): `AutoLod::build/generate/buildLevel`.
  A StaticMeshAsset is simplified at `Settings::ratios` (default 50/25/10 %) **per material section** (keeps the
  MSEC contract), written as `<stem>_LOD<n>.hasset` beside LOD0 via `Importer::writeAsset` with no source file
  (a sidecar has no "Reimport"). Levels that do not shrink below `minStepRatio` of the level above are dropped.
  Per-level `worldError` = meshopt relative error x section extent (object-space metres).
  The LOD0 link is **the name convention only** (`lodPath`, `findLodSidecars`, `isLodSidecarPath`); no chunk, no
  HAsset change. Scenes store the sidecars' UUIDs themselves.
* `src/HE_Core/include/Math/LodDistance.h`: `lodDistanceForError`, `lodDistancesFromErrors`. Distance =
  error * 1080 / (2 tan(fov/2) * tolerancePx); defaults 60 deg, 2 px; made strictly increasing.
* `src/HE_Scene/include/HorizonScene/AutoLodApply.h` (header-only): `fillFoliageLayer` (FoliageLod entries,
  density 1/1/0.75/0.5/0.3 floor, hysteresis = tightest band / 10 clamped 1..10 m, levels beyond `drawDistance`
  are dropped, `drawDistance` is never changed, optional cross card last) and `fillLodComponent` (levels[0] = LOD0).
* Importer: `MeshImporter::ImportSettings::{generateLods, lodRatios, lodFoliage}` (default off; `finishImport`
  calls the generator after the LOD0 write); `Importer::ImportOptions::generateLods` is `optional<bool>`, unset =
  `Importer::autoLodOnImport()` (process default, off). `reimport()` regenerates when `<name>_LOD1` exists
  (ratio list cut to the existing count; the real ratios are not stored).
* Editor: `AutoLodDialog.{h,cpp}` (dialog + `run()`), wired in `EditorUI.cpp` (draw), Content Browser mesh menu
  "Generate LODs...", Inspector Foliage "Detail Levels" button (`Generate LODs##fol`) and LOD component button
  (`Generate LODs##lod`), Preference `EditorConfig::AutoLodOnImport` (load/save in EditorApplication, catalog row,
  Settings panel row, Restore Defaults; applied each frame via `Importer::setAutoLodOnImport`).
  Undo: `snapshotNow("Generate LODs")` before the scene edit only; sidecar files stay on disk.
* Help: `EditorHelp.cpp` entries (Foliage/LOD/Content Browser/Generate LODs/*, Preferences). `editor_help_audit.py
  --check` was run and passes (1248/1248).
* Tests written: `tests/test_auto_lod.cpp` (ratios, distances, sections, foliage mode, sidecars + UUID stability,
  foliage/LOD fill, old-scene load + round trip), `tests/test_auto_lod_ui.cpp` (dialog open/Escape, option rows,
  run() for layer/entity/refusals, Undo). Both registered in `tests/CMakeLists.txt` (UI test in the main list and
  the per-file `HE_IMGUI_ENABLED` list, plus `AutoLodDialog.cpp`).

## Handoff

- **Topic / step:** 331, step 3 of 7
- **Branch / commit:** claude/auto-lod-und-sichtbarkeitsgetriebenes-rendern-f-r-foliage-un (WIP commit, pushed: see the Hive post)
- **Done:** all code above is written. Help audit passes.
- **Verified:** only `editor_help_audit.py --check` (green). The `he_tests` build started with `test_auto_lod.cpp`
  in the list and got to ~1300/1709 before the stop; no compile result for any new file was read.
- **Not verified:** that anything compiles (AutoLod.cpp, AutoLodApply.h, AutoLodDialog.cpp, InspectorPanel/
  ContentBrowser edits), that any new test passes, the full `he_tests` run, the baseline measurement (triangles per
  pass vs. step 1), a screenshot of the dialog, the asset_compiler (untouched; has no `--lods` flag).
- **Decisions taken:** name convention instead of a LOD0 chunk (no format change, scenes hold UUIDs anyway; cost:
  renaming LOD0 in the browser leaves the sidecars under the old name, so Reimport/regenerate will not find them);
  buttons live in the Inspector (Foliage Detail Levels, LOD component), not in the Landscape tab; default off for
  import; cross card is opt-in because it draws the layer's material until the step-4 impostor gives it a picture;
  levels past `drawDistance` are dropped rather than moving the user's cut-off.
- **Decisions suggested, not taken:** a `--lods` flag for asset_compiler; storing the ratios in LOD0 (tail chunk
  `MLOD`) if rename-safety matters; a Landscape-tab shortcut button.
- **Pending:** compile + fix, run `he_tests --source-file='*test_auto_lod*'` then the full run (no `--no-skip`),
  UI screenshot (`HE_UI_DUMP_DIR`, `scripts/he_uishot.py`), measurement, mark the topic.
- **Traps:** the `grep` in this shell silently returns nothing for some patterns, use `/usr/bin/grep`. No ninja in
  PATH: use `/Applications/CLion.app/Contents/bin/ninja/mac/aarch64/ninja` (configure recipe in memory
  `headless-dump-log-and-worktree-configure`; configure takes ~9 min, it fetches meshoptimizer). The tree is
  `out/build/release`; I left a `cmake --build ... --target he_tests` running (a classifier denied `pkill`), log
  `/tmp/auto-lod3-build.log`. Files edited while it compiled and so possibly stale: `MeshImporter.{h,cpp}`,
  `ImporterCommon.{h,cpp}`, `ContentBrowserPanel.cpp`, `InspectorPanel.cpp`, `EditorApplication.cpp`,
  `EditorSettings*.cpp`, `EditorHelp.cpp`, `EditorUI.cpp`: `touch` those `.cpp` files (only them) before rebuilding.
  The CMake edits (`tests/`, `src/HE_Editor`, `src/HE_Tools`) cause one reconfigure.
- **Next steps:** 1. wait for or restart `cmake --build out/build/release --target he_tests -j8`, fix compile errors.
  2. run the two new test files, then the full suite. 3. build `HorizonEditor`, open the dialog once, screenshot.
  4. Measurement: a small env-guarded harness (or the editor) that runs `AutoLodDialog::run` on the 12 layers of a
  clone of `~/HorizonEngineTestAssets/foliage-test-project` and `scripts/he_perf_capture.py --scene` (read the
  baseline doc's lessons first: always pass `--scene`, APFS clone, `counters_midframe.tris` is the main pass only).
  5. Step 4 can then bake the impostor into the cross-card level (`FoliageOptions::crossCard`).
