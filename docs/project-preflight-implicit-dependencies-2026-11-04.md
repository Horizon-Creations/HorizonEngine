# Project preflight: dependencies that were not loaded (2026-11-04)

User report: "die engine lädt nicht richtig die dependencies beim laden des projects bei dem
kleinen dialog" (the progress dialog of `ProjectPreflight`, `src/HE_Editor/ProjectPreflight.cpp`).

## What went wrong (from the user's own editor log, `HorizonEngineBuild/Editor/HorizonEngine.log`, 4 Nov 17:29-17:38)

Project `HEProject` has a terrain with `groupMix: true` and `material: {asset: [1188, 1]}` (the mix material).

1. `Project preflight: 1 engine asset(s) referenced, 1 already here, 0 downloaded, 0 missing` — the dialog
   said "all fine". The walk (`HE::EngineDeps::resolve`) maps a scene's asset ids through the **server
   catalogue** only. `Engine/Materials/M_AutoLandscape_Mix.hasset` ships with the editor
   (`Editor/EngineContent/Materials`) and is **not in the 99-entry server catalogue**, so id `[1188,1]` was
   dropped, the material was never scanned, and its three arrays
   (`T_Landscape_Mix_{Albedo,Normal,Mask}_Array`, ~1 GB each, which ARE served) were never requested.
2. The same startup refresh removed those three cached arrays as out of date ("downloads again when it is
   used"), so they really were missing.
3. The promised on-demand download never started. Checked: the cached manifest holds proper ids for the three
   arrays (1189..1191), so the registration loop did not skip them. What fits the log: the startup probe thread
   removes the stale files (17:29:59.96), calls `refreshEngineFolder` (bumps the engine folder version) and then
   sets `m_sftpManifestReady`. `OnRender` registers the remote entries (EditorApplication.cpp ~3538) BEFORE
   `EditorUI::render` re-scans the disk index on the version bump (EditorUI.cpp ~801), so
   `registerRemoteAsset` still saw the stale ids in `m_diskRegistry` and returned early; the later re-scan
   dropped the ids but nothing registers remote entries again. Afterwards `requestRemotePath` finds no path
   entry and every load ends in `Cannot load asset … no readable .hasset`
   (the Mask array only stopped erroring when it was fetched by hand, 17:38).
   **Inferred from code order and log timestamps, not traced at runtime.**

## Fix

- `EngineDependencies.cpp`: a scene id the catalogue does not know is looked up in the library files on
  this machine (project override, `engineRoot`, cache) — `peekAssetId` reads the id from the META chunk,
  index built lazily once per walk (`Walker::indexLocalIds`). The material is then scanned like any other
  and leads to its arrays. The `.hescene` parse no longer needs a non-empty catalogue.
- `ContentManager.cpp`: `registerRemoteAsset` treats a disk-registry entry whose file is gone as absent
  (erases it, registers the remote); `loadAssetAsync` does the same when a remote entry exists.
- No dialog controls changed, so no `EditorHelp.cpp` entry.

## Handoff

- **Topic / step:** project open / EngineContent preflight, bug fix
- **Branch / commit:** claude/runtime-virtual-textures, **not committed** (task did not ask for it)
- **Done:** the walk now reaches assets referenced by id that only the editor ships; a stale index entry no
  longer blocks the on-demand download.
- **Files touched:** `src/HE_Core/src/ContentManager/EngineDependencies.cpp` (peekAssetId, Walker::indexLocalIds,
  scene id branch), `EngineDependencies.h` (comment), `ContentManager.cpp` (registerRemoteAsset,
  loadAssetAsync disk branch), tests: `tests/test_engine_dependencies.cpp` ("a scene's id for a library
  asset the server does not list…", "a library id that nothing … knows"), `tests/test_contentmanager.cpp`
  ("a file indexed and then deleted is asked for again…"). No `tests/CMakeLists.txt` change.
- **Verified:** both new positive tests FAIL on the old code (0 referenced / 0 downloads; materializer never
  called) and pass on the new; `he_tests --source-file=test_contentmanager,test_engine_dependencies,
  test_landscape_foliage_sync,test_landscape_reload`: 112 cases / 1478 assertions pass (Debug, scratch
  APPDATA). The real `M_AutoLandscape_Mix.hasset` carries META id `1188,1` in the layout `peekAssetId` reads.
- **Not verified:** no editor run (it would download ~2 GB of arrays; needs an own build tree with
  -DDEPLOY_DIR); the exact ordering of startup index vs. manifest consumption was inferred from code and the
  log timestamps, not traced. Only the Debug `he_tests` tree was built; a Release/editor build was not.
- **Decisions taken:** read ids from the files on disk instead of a hard-coded terrain rule — it also covers
  every other shipped-but-unpublished asset a scene names by id.
- **Suggested, not taken:** code-bound dependencies that appear in NO file (a script/HorizonCode that
  loads a path built at runtime; the terrain's group *materials* when a scene stores no material) are still
  invisible to the preflight. Possible: a registry of implicit engine paths per component.
- **Traps:** `tests/HorizonCore.dll` is a copy that is NOT refreshed when only HE_Core changes — copy
  `out/build/x64-debug/src/HE_Core/HorizonCore.dll` over it or the tests run the old code.
  The startup probe and the preflight both call `refreshManifestBlocking` concurrently (two "99 entries"
  lines); harmless here, but both can run the stale-cache removal.
  `ProjectPreflight` reports `NoFetcher` ("this build cannot download") when only a cached catalogue exists
  and the server is unreachable — misleading text, unchanged.
- **Next steps:** 1. Build editor in an own tree, open a copy of `HEProject` with a scratch APPDATA seeded with
  the user's cache; expect "referenced" > 1 and downloads of the Mix arrays. 2. Decide on the implicit-path
  registry above.
