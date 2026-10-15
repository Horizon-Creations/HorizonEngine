# Collab sync audit, step 8: save and idle hook (2026-10-14)

Topic 198, step 8. Task: in a live session, the state goes to the "server" when somebody saves a
file and as soon as nobody is working on it anymore. With the source-control mediator
(topic 199) switched on, the server commits and merges from there.

## 1. What "the server" is here

Topic 199 is only a design so far (`docs/team-server-source-control-design-2026-10-13.md` on
branch `claude/team-server-als-source-control-mittelsmann-opt-in-auto-merge`; no
`TeamSyncService`, no `sourceControl.mode` yet). Its §2 says that inside a session **only the
host** syncs and clients pause. So for this step the host's disk is the session's state:
joiners are caught up from it (step 7) and source control and the team server read it. This step
builds the moment ("kept") and the attach point. It does not build the upload.

## 2. What was there before (verified by reading)

| Document | On the host's disk? | Gap |
|---|---|---|
| Asset graphs, materials, widgets... | yes: the holder's 1 s whole-file autosave arrives and is written | no "this is a save" vs. "autosave traffic" signal |
| GameInstance graph | yes: written on every applied delta | none |
| Scene (entities, components, landscape, Level Script) | **no**: live in the host's world, unsaved until the host presses Save | a client's Ctrl+S wrote the **client's** disk only (into the scene path it had open *before* joining, since the snapshot does not move `m_currentScenePath`); the host never heard of it |
| "Nobody works on it anymore" | nowhere | no idle notion at all |

## 3. What now happens

* `src/HE_Editor/CollabSettle.h` (pure, clock-driven): per key `dirty` + `saved`.
  **Saved**: an explicit save anywhere in the session, batched `kSaveBatchMs` = 1 s (Save All is
  one batch). **Idle**: no change to that key for `kIdleMs` = 5 s. `::LevelScript::` settles as
  `kSceneKey` (`::Scene::`).
* Host only. Changes are noted at `CollabController::noteSessionTouched` (assets, own and peer),
  `noteSessionOp` (delete/rename: old and new path) and `noteSceneTouched` (every entity
  channel, own publish and peer receive: transform, components, structural, terrain, entity op).
* `CollabController::updateSettle` (from `update`) runs a **persist** step per settling key
  (`setSettlePersist`), then calls `onSessionSettled(batch)` and logs
  `Collab: session state kept: <key> (saved|idle), ...`.
* Editor persist (`EditorApplication.cpp:730`): the scene → `persistSessionScene()` writes the
  host's **real scene file** via `saveSceneToPath`. Other keys → true (already on disk). Returns
  false in Play, for an unnamed scene, with the dump path set, or while `SceneDiskWatch` blocks the
  save. The key then stays pending and is tried again on the next frame.
* Save signals: `ContentManager` save hook → `noteLocalSave(rel)` unless it is the session
  autosave (`m_collabAutosaveWrite`, set around `EditorUI::saveAsset` in
  `updateAssetCollabSync`); `saveSceneToPath` → `noteLocalSave(kSceneKey)` unless it is the
  settle write itself (`m_settleWritingScene`). A client sends **`kMsgDocSaved`** (new,
  `CollabSession::sendDocSaved`, client → host, string key); the host answers its own inline.
  **Protocol v16**.

## 4. Decisions

* Locks are **not** "working". An asset lock is held while its tab is open and an entity lock
  while it is selected, often for hours. Quiet time per key is the signal. 5 s covers several
  1 s autosave periods, so the last whole file has landed on the host.
* The host writes the real scene file on settle, not a copy under `Saved/`: topic 199's
  uploader scans the project tree and would never see a copy. The host's title-bar `*` clears.
* Pending keys are dropped at `teardown()`: after the session the host edits alone, and its next
  save is an ordinary one (199's normal save trigger uploads everything that differs).
* No new UI control (the help audit stays at baseline). Step 9 adds the panel notice.

## 5. Known limits

* A client's explicit save of an asset it does not hold yet: `publishAsset` waits for the
  grant, so the "saved" notice can reach the host before the file. The batch then reports the
  key with the previous bytes on disk. The arriving file re-arms the key, and it settles again as
  Idle 5 s later with the right bytes.
* A client's Ctrl+S still also writes the client's own scene path (unchanged; possibly the wrong
  file, see section 2). Not fixed here.
* C++ class saves do not pass through the ContentManager hook, so they settle only as Idle.
* The host's own Ctrl+S on the scene writes it twice: once for the save, and once more 1 s later
  when the Saved batch settles and `persistSessionScene` runs. That is waste, not a loop
  (`m_settleWritingScene`). Do not skip the write on `m_undo.revision() == m_savedRevision`:
  peer edits do not necessarily bump the undo revision.
* Checked: the GameInstance graph (`saveGameInstanceGraph`, raw `ofstream`) and the Material
  Editor (`saveAsset` only from its explicit `saveToDisk`) do not turn edit traffic into Saved.
  Any other new writer through `ContentManager::saveAsset` in a session now counts as a save
  unless `m_collabAutosaveWrite` is set around it.
* `saveSceneToPath` runs `syncPrefabInstances("save")`. If that mutates the world on every call,
  the host would re-save every 5 s. Not observed and not tested live.

## Handoff

- **Topic / step:** 198, step 8 of 11
- **Branch / commit:** `claude/audit-vollst-ndige-bestandsaufnahme-was-ber-live-collab-sitz` @ 61b13c7b (code) + this doc
- **Done:** host-side save/idle settle with a persist step and the `onSessionSettled` hook; client saves reach the host (v16).
- **Files:** `src/HE_Editor/CollabSettle.h`; `CollabController.cpp:1449` onDocSaved, `:1980` updateSettle call, `:4438-4470` noteSceneTouched/noteLocalSave/updateSettle, `:4426`/`:4482` change notes, `:1781` reset; `CollabController.h` public block after `publishAssetCreate`; `EditorApplication.cpp:707` save hook, `:730` persist, `:9782` autosave flag, `:12982` scene notice, `:12994` persistSessionScene; `CollabSession.{h,cpp}` sendDocSaved/handleDocSaved, v16.
- **Verified:** out/build/release `he_tests` + `HorizonEditor` build rc 0. `test_collab_settle.cpp` 7 cases/26 assertions; `test_collab_controller.cpp:3752` (real host+client on loopback) green. Negative controls: without the host's `onDocSaved` handling, and without `noteSceneTouched` on a received transform, the case fails at :3808 / :3836. Full he_tests minus material_graph (`HOME=$(mktemp -d)`): 5140/5140. `editor_help_audit.py --check` 1168/1168.
- **CI:** https://github.com/Horizon-Creations/HorizonEngine/actions/runs/38631279496 started at handoff. The step-7 run 38625018474 was red on Windows only: `test_collab_controller.cpp:3691` `REQUIRE(annaFile.string() == disk.path(brick))` (step 7's catch-up test, most likely a path separator). Not touched here, so expect it again.
- **Not verified:** no live two-editor run. `persistSessionScene` and the save hook flags are build-checked only (EditorApplication.cpp is not in he_tests). The relay path was not exercised.
- **Decisions taken:** section 4.
- **For step 9:** wire `m_collab.onSessionSettled(...)` in `EditorApplication` (single slot, currently unused; the controller logs). With 199's `mode = "teamServer"`, request the upload there (debounce is already done). Show the notice in `SourceControlPanel`; `lastSettledMs()` / `lastSettledCount()` / `settlePending(key)` are there for a status line.
- **Traps:** `pumpUntil` restarts its clock at 0 on each call, so settle tests need their own monotonic clock (see the test's `tickUntil`). CollabSession.h edits rebuild ~75 he_tests steps.
- **Next steps:** 1. Step 9 as above. 2. Step 11 live: Bob saves the scene, and Anna's scene file changes and her `*` clears. Nobody touches anything for 5 s, and Anna's log shows `session state kept`.
