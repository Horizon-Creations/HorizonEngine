# Collab sync audit, step 4: HorizonCode graphs (2026-10-14)

Topic 198 step 4. Inventory read from the code (not run live), and the implementation of the
gaps it found (commits d1c7b20c, 29d58a22). Built and tested locally; no live two-editor run.

## 1. How a HorizonCode graph travels today

Two channels (EditorApplication.cpp `updateAssetCollabSync` ~9628, `publishDocDeltas` ~9718,
`applyRemoteDocDeltas` ~9756):

* **Document deltas** (`CollabDocSync`, per item, diff against a mirror). Sent only while this
  editor HOLDS the asset lock. The receiver patches the live graph.
* **Whole-file autosave** (1 s, lock holder only, `ContentManager::saveAsset` → `publishAsset`).
  The receiver writes the file and `reloadAssetTabFromDisk` re-reads the open tab (mirror reset).

Hosts of an HC graph and how each one is bound:

| Host | Delta binding | Whole-file channel |
|---|---|---|
| Class asset (`HorizonCodeClassPanel`, LevelScriptPanel.cpp ~2808) | yes | yes |
| Widget logic graph (`UIEditorPanel` ~6791, `Scope::LogicGraph`) | yes | yes |
| Level Script (editor-owned, EditorApplication.cpp ~9609) | yes | **none** (lives in the scene) |
| Game Instance graph (editor-owned, ~9617) | yes | **none** |
| Animator sync graph (AnimatorStateMachineEditorPanel.cpp `syncGraph`) | **was missing** | yes |

## 2. What the HC adapter carried (CollabDocSync.cpp `HcAdapter`)

| Operation | Carried as | Before this step |
|---|---|---|
| Create / move / delete node, change a literal or pin default | `Kind::Node` (full `nodeToJson`) | yes |
| Links | `Kind::Link` | yes |
| Create / change / delete variable, function-local variable (`scope`) | `Kind::Variable`, keyed by name hash | yes |
| Rename variable | remove + upsert + node upserts | yes (test added) |
| Create / change function (FunctionEntry node, params), calls follow | `Kind::Node` + `afterApply` → `syncFunctionSignatures` | yes |
| Declared Custom Event (create / change arg / delete) | — | **no** |
| Comment boxes | — | **no** |
| Extract on Destruct spec | — | **no** |
| Class-level fields (baseClass, components, name) | whole file only | (by design) |

Severity: for class / widget assets the missing items arrived about 1 s later through the autosave
(with a tab reload). For the **Level Script and Game Instance** they never arrived.

## 3. Bigger finding (fixed in the resumed run): editor-owned graphs never claimed the lock

`beginAssetEdit` has exactly one caller (EditorApplication.cpp:9669) and it is gated on
`EditorUI::tabHasUnsavedEdits`, which is false for `LevelScriptPanel::kTabPath` and
`GameInstancePanel::kTabPath` (EditorUI.cpp:961, no panel answers for them). So a human edit in
those tabs never requests the lock, `ownsAssetLock` stays false and `publishDocDeltas` only seeds.
**Human edits to the Level Script / Game Instance in a session do not reach peers at all**
(MCP edits only check `lockedByOther`, ~9961). Read-only banners do work for those keys
(EditorUI.cpp ~3887). Verified by reading only.

Fix: `CollabDocSync::hasLocalChanges(doc, mirror)` (compares the document with the mirror,
read-only) is the missing dirty flag. `updateAssetCollabSync` calls `m_collab.beginAssetEdit(key)`
for the two editor-owned keys when it is true and we do not hold the lock yet; the edit stays
in the mirror and `publishDocDeltas` (alwaysDiff) sends it once the grant lands. Covered by
the adapter-level test only (`EditorApplication` is not in he_tests); no live two-editor run.

## 4. Implementation

* `HorizonCode.h/.cpp`: public `eventDeclToJson/FromJson`, `commentToJson/FromJson`,
  `extractToJson/FromJson` (empty spec = `"{}"`); `toJson`/`fromJson` now built on the same
  object helpers, so the item form and the file form cannot drift.
* `CollabDocSync.h`: `Kind::Event`, `Kind::Setting` appended; `kSettingExtract = 1`;
  `hasLocalChanges` (section 3).
* `CollabDocSync.cpp` `HcAdapter`: enumerate/upsert/remove/reorder for Event (name hash),
  Comment (id, `bumpNextId`), Setting (Extract, replace only).
* Animator panel: second binding `Scope::LogicGraph` → `syncGraph` with `collabSyncMirror`,
  reset in `reloadFromDisk` and `reloadByContentPath`.
* EditorApplication.cpp `updateAssetCollabSync`: lock claim for the two editor-owned graphs.
* `kCollabProtocolVersion` 14 → 15 (a v14 peer would skip the new kinds silently).
* tests/test_collab_docsync.cpp: 7 new cases (events, comments + nextId, extract set/clear,
  function + local + value edits, variable rename, animator two-document isolation,
  hasLocalChanges).

## Handoff

**Done:** inventory (sections 1-3); gaps of section 2, the animator binding and the section-3 lock
claim implemented. Commits d1c7b20c, 29d58a22.
**Verified:** `he_tests` and `HorizonEditor` build (out/build/release). Green: test_collab_docsync
(24 cases), all `test_horizoncode*` (215), test_collab_controller (79), test_collab_undo,
test_mcp_hc, test_mcp_tools_collab, test_editor_commands, test_terrain_sync, test_net_collab,
test_net_lanbeacon. Full he_tests (minus material_graph): see the step's result post.
**Not verified:** no live two-editor session; the animator panel binding and the Level Script /
Game Instance claim are covered only at adapter level (neither file is in he_tests).
**Decisions:** reuse `HcAdapter` + the existing delta wire, not a new path; Extract as an
always-present singleton (replace only); protocol bump to 15; editor-owned graphs claim on
"document differs from mirror" instead of getting a dirty flag.
**Pitfalls:** `HorizonCode.h` and `CollabSession.h` rebuild ~410 targets (~35 min at -j8, load 3).
`CollabDocSync::Kind` values are wire bytes: append only.
**Next steps / open for later steps:**
1. Step 6: `HcRenameSweep` writes other assets via `saveAsset` without checking their locks;
   lock-deny on the editor-owned graphs leaves the loser's edit as a fork (`onAssetLockDenied`
   reloads from disk, which those two keys don't have).
2. Step 7: check late join / reconnect for the Level Script and Game Instance graph. During a
   session deltas are their only channel; whether the join snapshot carries them was not checked.
3. Step 11: a live two-editor run that edits each HC host.
