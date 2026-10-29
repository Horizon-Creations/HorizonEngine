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
- **Branch:** claude/auto-lod-und-sichtbarkeitsgetriebenes-rendern-f-r-foliage-un
- **Done:** all code above compiles and its tests pass. Fixes in the finishing run: `AutoLodDialog.cpp` was missing
  from the he_tests source lists (link error; now in `tests/CMakeLists.txt`), `test_auto_lod_ui.cpp` needed
  `using namespace HE::Ed` and must not call `ImGui::IsPopupOpen(const char*)` between frames (null current
  window, SIGSEGV; it now looks the popup up in `OpenPopupStack`), `EditorHelp.cpp` needed an area rule
  `"Generate LODs/"` (test_editor_help requires every key to have one).
- **Verified:** `he_tests` built (Release, out/build/release); `--source-file='*test_auto_lod*'` 14/14 pass; full
  `he_tests` run from `out/build/release` 5982/5983, the one failure is `test_hpak` "Disk registry: scanContentDirectory
  resolves UUIDs" (7 == 2): `scanContentDirectory` also indexes `~/Library/Application Support/HorizonEngine/
  EngineContentCache` which holds 5 synced Landscape textures on this Mac; environmental, not this branch.
  `editor_help_audit.py --check` rc 0. Dialog screenshot looked at (HE_UI_DUMP_DIR, he_ui bmp): layout fine.
- **Not verified:** HorizonEditor target not built (he_tests compiles the panels and the dialog, the editor exe was
  not linked/run); baseline measurement (triangles per pass vs. step 1) not done; asset_compiler has no `--lods` flag;
  CI for the new commits.
- **Decisions taken:** name convention `<stem>_LOD<n>.hasset` instead of a LOD0 chunk (renaming LOD0 in the browser
  orphans the sidecars); buttons in the Inspector (Foliage Detail Levels, LOD component); import generation default off;
  cross card opt-in (draws the layer's material until the step-4 impostor); levels past `drawDistance` dropped;
  Undo covers the scene edit only, not the sidecar files.
- **Decisions suggested, not taken:** `--lods` for asset_compiler; `MLOD` tail chunk in LOD0 for rename safety;
  Landscape-tab shortcut button.
- **Traps:** `/usr/bin/grep` (the wrapper returns empty); ninja at `/Applications/CLion.app/Contents/bin/ninja/mac/
  aarch64`; run he_tests from `out/build/release`, not from `tests/`; do not use `IsPopupOpen(const char*)` outside a
  window in headless UI tests.
- **Next steps:** 1. measure on an APFS clone of `~/HorizonEngineTestAssets/foliage-test-project`
  (`he_perf_capture.py --scene`, read the baseline doc's lessons: always `--scene`, main-pass tris only).
  2. build HorizonEditor and open the dialog once for real. 3. Step 4 bakes the impostor into the cross-card level
  (`FoliageOptions::crossCard`).
