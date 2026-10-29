# Asset identity: panel documents are filed by UUID, not by the tab's path string (2026-10-29)

Trigger: a two-editor collab test where landscape edits arrived but HorizonCode class edits were
not visible. Reading the code found one concrete way for that to happen, and a second, related
defect in the same place (rename/move). Not reproduced live.

## 1. What was wrong

Every asset panel keeps its open document in an `AssetPanelState<T>` map (18 panels), which was
keyed by the **exact tab path string**.

* **Collab receiver.** A peer's delta / file arrives as a content-relative key. The editor turns it
  back into a path with `collabLocalPath` → `ContentManager::resolveSavePath` (`root + "/" + key`) and
  looks the panel up with it (`applyRemoteDocDeltas`, `reloadAssetTabFromDisk`). The tab itself was
  opened with the Content Browser's `it->path().string()` (native separators on Windows). Any
  difference in spelling = `find()` misses = "nobody has that tab open" = the file lands on disk,
  the open tab shows nothing. Landscape/scene sync does not go through this lookup, which fits the report.
* **Rename / move.** `EditorTabs::retarget` repoints the tab, nothing repointed the panel state: the
  edits, undo history and dirty flag stayed under the old path, a fresh empty state grew at the new one,
  and the quit prompt listed the old path.

## 2. What changed

* `src/HE_Editor/AssetIdentity.{h,cpp}` (new). `keyFor(path)`: `uuid:<hex>` from the file header
  (`AssetRefs::assetUuidOfFile`), else `path:<normalised>`, virtual tabs (`:Settings`) unchanged. A UUID is
  accepted only if `ContentManager::pathForId` does not place it in a *different* file (a copy made outside the
  editor carries the original's id). Cached per spelling, re-checked against mtime/size every 250 ms.
  `bind(&contentManager())` in `EditorApplication.cpp` (next to `setOnAssetSaved`).
* `EditorPanelState.h`: `AssetPanelState` stores `Entry{path, state}` under the identity key.
  `find/[]/forget/dirty` take any spelling; `forEach/appendDirtyPaths` hand out the *current* path.
  When a state is asked for under a different path (a move), `onMoved` refreshes `relPath`, or calls
  `State::onAssetMoved(newRel)` if the State has it. A state filed under `path:` before the file had a UUID is moved
  (same object) to the UUID key.
* `LevelScriptPanel.cpp`: `ClassState::onAssetMoved` also re-files the canvas view state (`s_graphStates`) and the name.
* CMake: `AssetIdentity.cpp` in HorizonEditor and in `he_tests`; `tests/test_asset_identity.cpp` (9 cases).

## 3. Not done (and why)

* **The collab wire and the lock subject are still path-based** (`CollabController::assetSubject` = FNV-1a of the
  project-relative path; `collabSyncKey`; every asset message carries the path). Moving them to UUIDs is a protocol
  change (`kCollabProtocolVersion` 17 → 18, mixed-version peers) with real design questions: an asset one peer has and the
  other does not has no UUID there yet (new assets, creates), C++ sources and folders have none, rename arbitration
  needs both ids. Not attempted without a live two-editor run.
* Other path-keyed editor maps outside `AssetPanelState` (`m_assetLastAutosaveMs`, `m_docMirrorPaths`, thumbnail caches, `s_graphStates`
  of other panels) were not touched; they are keyed by the tab's own string consistently, so they restart after a rename (harmless).
* `State::name` (cosmetic) is not refreshed on a move, except for the HorizonCode class.
* Engine default vs. its project override share one UUID, so their two tabs now share one state (they were already
  treated as one asset by `EditorTabs::sameAsset`).

## Handoff

- **Topic / step:** collab sync, asset identity (follow-up to `collab-sync-audit-horizoncode-2026-10-14.md`)
- **Branch / commit:** working tree of `/home/user/HorizonEngine`, not committed, not pushed
- **Done:** panel documents are found by asset UUID under any path spelling, and survive rename/move with their edits.
- **Files touched:** `src/HE_Editor/AssetIdentity.{h,cpp}`, `EditorPanelState.h`, `LevelScriptPanel.cpp` (`ClassState::onAssetMoved`),
  `EditorApplication.cpp` (bind), `CMakeLists.txt` (editor), `tests/CMakeLists.txt`, `tests/test_asset_identity.cpp`.
- **Verified:** `he_tests` and `HorizonEditor` build (Linux, Release). `test_asset_identity.cpp` 9 cases / 46 assertions green.
  Full `he_tests` (minus material_graph): 5835 of 5837 cases pass; the 2 failures are `test_net_discovery.cpp` (`socketDefaultGateway`,
  `socketOwnsAddress("::1")`): the container has no private gateway and no IPv6 loopback, unrelated to this change (not run on the
  base commit). `editor_help_audit.py --check` 1243/1243.
- **Not verified:** no live two-editor run, so whether this was the cause of the HC class report is **unconfirmed**; the
  Windows backslash spelling itself is argued from the code (the test uses `..` and `//` spellings, Linux has no `\`).
  No UI run of a rename with an open dirty tab.
- **Decisions taken:** identity = file UUID checked against `ContentManager::pathForId`, fall back to normalised path; state keeps
  its object across a move (panels hold references into it); wire/locks left on paths.
- **Decisions suggested, not taken:** UUID on the collab wire + lock subject (see §3).
- **Traps:** `AssetPanelState` is used by 18 panels, so `EditorPanelState.h` rebuilds all of them. A State with `relPath` is
  refreshed automatically; one that caches a path under another name must declare `onAssetMoved`.
- **Next steps:** 1. Live two-editor run: edit an HC class on both sides, rename it mid-session. 2. Decide the wire/lock change
  (§3). 3. Add diagnostic logging in `updateAssetCollabSync`/`applyRemoteDocDeltas` ("nobody has that tab open" is silent today).
