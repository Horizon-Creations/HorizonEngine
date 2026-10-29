# Asset identity: panel documents, collab locks and frames are addressed by UUID (2026-10-29)

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

## 3. Collab wire: locks and frames by UUID, protocol 18

Second step of the same topic, on the user's request ("locks per id, changes referenced per id,
nothing has to be touched when a name or path changes; make the same-project check better").

* **Lock subject.** `CollabController::subjectFor(key)`: when the identity resolver gives the asset a UUID the
  subject is `HE::Net::assetUuidSubject(hi, lo)` (folded UUID, top bit set), else the old path hash
  (`assetSubject`). Every claim, release, query and frame goes through it (21 call sites switched). A rename
  therefore changes nothing the host's lock table holds: no release, no re-claim.
* **Incoming frames** (`onDocDeltas`, `onAssetUpdated`) are resolved by their subject to this machine's
  CURRENT key (`resolveIncomingKey` → editor `keyForCollabSubject` → `ContentManager::pathForId`), and only
  fall back to the path in the frame when we do not have that asset (a create, a new asset). Revision counters
  are filed per subject for UUID assets (`revisionIdentity`), so they survive a rename.
* **Bookkeeping by key** (held locks, subject→key table, pending releases, answers, edit requests, session-touched)
  is moved by `rekeyAsset` after an approved rename has been carried out. It only follows entries whose NEW key
  still gives the same subject, i.e. the UUID-addressed ones; a path-addressed asset keeps the old behaviour.
* **Resolvers** are wired in `EditorApplication.cpp` next to `setLocalPathResolver`: key → file →
  `AssetIdentity::uuidFor` (the file header, duplicates excluded); subject → UUID via an index over
  `ContentManager::projectAssetIds()` (new accessor), rebuilt on `contentEpoch()` change or a miss
  (max once per 500 ms). The path is always asked of the ContentManager, never cached.
* **Protocol 17 → 18.** A v17 peer derives subjects from paths, so the same asset would be two locks across the
  builds; handshake equality check refuses the mix, as for every earlier bump.

### Same-project check (`CollabSession.h` `ProjectFingerprint`, join request)

The manifest id stays the primary check. A join now also carries a **fingerprint**: asset count + a bottom-k
(k = 64) sketch of the hashed asset UUIDs, appended to the request. The host estimates the Jaccard overlap and
tells three cases apart (both sides need at least 8 assets, else only the id counts):

| ids | overlap | verdict |
|---|---|---|
| equal | < 0.15 | `ProjectDiverged` (template copy / "Save As" with the same id) |
| differ | ≥ 0.6 | `ProjectIdMismatch` (same files, id minted separately; fix = same `id` in both .heproj) |
| differ | else | `ProjectMismatch` as before |
| equal | else | joins (two people on one project differ by what one has not pulled yet) |

Thresholds are mine, not measured on real projects. The wire reader bounds the sketch (<= 64, strictly ascending,
<= asset count) before allocating.

## 4. Not done

* Asset operations (`requestAssetOp`: rename/delete) still carry paths; they are arbitrated by the host by key.
  Only the lock and the content frames are by id.
* C++ sources, folders, legacy assets without a UUID and assets this machine has not received yet stay path-addressed.
* A lock claimed for an asset *created* mid-session (before the peer has the file) can be a path subject on the peer
  until the file arrives; the window is the create → transfer gap.
* Other path-keyed editor maps (`m_assetLastAutosaveMs`, `m_docMirrorPaths`, thumbnails) restart after a rename.
* `State::name` (cosmetic) is not refreshed on a move, except for the HorizonCode class.
* Engine default vs its project override share one UUID, so their two tabs share one state.

## Handoff

- **Topic / step:** collab sync, asset identity, steps 1 (panel documents) and 2 (wire, locks, project check)
- **Branch / commit:** `claude/collab-asset-identity-uuid`, PR #209 (step 1 pushed; step 2 see the PR)
- **Done:** panel documents by UUID under any path spelling and across a move; lock subject and frame routing by UUID
  (protocol 18); join compares an asset fingerprint next to the manifest id, with two new rejection reasons.
- **Files touched:** `src/HE_Editor/AssetIdentity.{h,cpp}`, `EditorPanelState.h`, `LevelScriptPanel.cpp`
  (`ClassState::onAssetMoved`), `CollabController.{h,cpp}` (`subjectFor`, `resolveIncomingKey`, `rekeyAsset`,
  `setAssetIdentityResolvers`), `EditorApplication.{h,cpp}` (resolvers, `keyForCollabSubject`, fingerprint),
  `src/HE_Net/{include/Net,src}/CollabSession.{h,cpp}` (fingerprint, reasons, `assetUuidSubject`, v18),
  `src/HE_Core/include/ContentManager/ContentManager.h` (`projectAssetIds`), tests below.
- **Verified:** `he_tests` and `HorizonEditor` build; `test_asset_identity` 9 cases; `test_net_collab` 122 cases (incl. 7
  new: overlap estimate, diverged / id-mismatch / mismatch / ahead-collaborator / too-small, subject); `test_collab_controller`
  104 cases (incl. 6 new: lock survives a rename, path-addressed unchanged, delta lands on the receiver's current key,
  unknown asset keeps the frame's path, held locks follow an applied rename). Full `he_tests` (minus material_graph): 5848 of 5850 cases; the 2 failures are the container-network ones in
  `test_net_discovery.cpp` (no private gateway, no IPv6 loopback), also red before this change set. `HorizonEditor` builds.
- **Not verified:** no live two-editor run (neither step); the overlap thresholds against real projects; behaviour of the
  new reasons in the join panel UI (text only); a rename arbitrated through a real client/host pair with files on disk.
- **Decisions taken:** UUID subject only when both the key and the registry agree it is that asset, else path; fingerprint
  is advisory below 8 assets; no auto-adopt of a peer's project id.
- **Decisions suggested, not taken:** carry the UUID explicitly in asset ops so a rename racing a delete is unambiguous.
- **Traps:** `CollabSession.h` and `ContentManager.h` rebuild ~260 targets (about 10 min at -j4 here). Tests that write a
  JoinRequest by hand (`writeJoinRequest` in `test_net_collab.cpp`) must append the fingerprint block.
- **Next steps:** 1. Live two-editor run: edit an HC class on both sides, rename it while one holds the lock.
  2. Check the join panel shows the two new messages. 3. Add logging for "nobody has that tab open" in `applyRemoteDocDeltas`.
