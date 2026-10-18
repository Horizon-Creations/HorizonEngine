# Collab sync audit, step 6: rename/delete conflicts go to the host (2026-10-14)

Topic 198, step 6. The pattern to apply everywhere: a delete or rename that destroys or moves
somebody else's work is **asked for**, the **host** answers in its in-tray (Collab panel), and
an approval is applied on every machine at once. This doc lists, per synced object type, where
that already held, where it did not, and what changed.

## 1. Inventory

| Object | Gesture | Before | Now |
|---|---|---|---|
| Asset file (graph, material, any `.hasset`, C++ class) | Content Browser delete / rename / drag-move, MCP `asset_delete` / `asset_move` | host asked (`requestAssetDelete/Rename`, batches) | unchanged |
| Asset file | Content Browser **Revert to Default** (deletes a project override) | **local only**: peers kept the override | host asked; every machine unloads the override on apply |
| Asset file a peer is **editing** | any delete / rename above | host row showed only the file name | row says "**Bob is editing it — their open changes go with it**" |
| Entity nobody else holds | delete / rename | applied + replicated (step 2) | unchanged: no conflict, no question |
| Entity a peer holds (or one under it) | Delete, Cut | refused with "try again once they deselected" | **host asked** (`requestEntityDelete`), one batch per keypress |
| Entity a peer holds | rename in Outliner or Details | renamed **locally only**, overwritten by the holder's next blob | **host asked** (`requestEntityRename`) |
| HC member rename reaching other graphs | Rename Across Project dialog | peer-held graphs "Skipped", lock read once at scan time | lock re-read every frame; "**Ask for it**" on peer-held rows (holder hands over, row becomes writable) |
| Graph items (nodes, variables, functions) | delete / rename inside one graph | under the asset's exclusive lock: nobody else can edit at the same time, so no conflict exists | unchanged |
| Material items (nodes, params) | delete / rename inside one graph | same: exclusive asset lock | unchanged |

## 2. How entity requests travel (no wire change)

`src/HE_Editor/CollabEntityOps.h`: the entity rides `CollabSession::AssetOp Delete/Rename`
unchanged under the key `entity:<16 hex subject>` (subject = uuid-derived wire id, identical on
every peer); on a rename `newPath` carries the new **name**. Same idea as `kSourceKeyPrefix`
for C++ classes. Protocol stays 15. Everything that reads `path` as a file branches first:

* `CollabController::onAssetOpApply` (`CollabController.cpp`): entity keys go to
  `onRemoteEntityOp`, never to `onRemoteAssetOp` or `verifyAppliedAssetOp`.
* `approveOne`: the occupied-name check is skipped for entities (an empty resolver answer
  used to mean "refuse").
* Notices: `OurOp::display` / `name()`, so footer and notes say "Crate", not the key.
* `EditorApplication` `onRemoteEntityOp`: delete = the `onRemoteDestroy` sequence
  (`m_selection.remove`, `forgetStructureOf`, `destroyEntity`); no structural message, the
  holder deselects and `followSelection` releases its lock. Rename = `renameEntity`; the
  holder's component blob carries the name afterwards.

**Host's own ask:** goes into its own in-tray (requestId 0), unlike its asset deletes, which
apply directly because the Content Browser already showed a confirmation dialog. An entity
delete is one keypress; the row (with the holder's face and name) is its confirmation.

## 3. Not done here (by decision)

* Asking about every entity delete: no. Unheld entities are deleted at once (step 2); asking
  the host for each would make a session unusable. Only the conflict is asked about.
* MCP entity destroy of a peer-held entity (`EditorCommands`) still refuses (`locked_by_other`);
  the client can retry. A `requested:true` answer like `asset_delete` would be the next move.
* Asset rename while a peer holds it: the holder's tab follows the file (`EditorTabs::retarget`)
  but its lock was keyed on the old path. Not changed.
* Items tagged "step 6" by earlier workers that are not rename/delete conflicts: material
  lock-denied fork (materials doc section 4), undo-floor reset for HC/particle/animator/widget
  panels, landscape stroke undo (terrain doc section 4), human structural gestures on
  `CollabUndo`. All still open.
* No protocol bump although an asset-op `path` can now mean an entity: all 0.7.0 builds ship
  together, and a bump means a `CollabSession.h` edit (~35 min rebuild). An older build of the
  same protocol would see an `entity:` row as a file and its apply would find nothing on disk.
  The queen can overrule this with a bump to 16.
* The holder gets no notice of its own when an approved delete takes its open asset tab; only
  the host's row warns beforehand.
* Sweep writes on a **client** to graphs nobody holds (and graphs handed over by "Ask for it"): `saveAsset` → `publishAsset` requests
  the lock and does not re-send after the grant (same as the MCP case in the materials doc).
  Lost-update territory, step 7.

## Handoff

**Done:** `CollabEntityOps.h` (new), `CollabController.h/.cpp` (`requestEntityDelete/Rename`,
`onRemoteEntityOp`, `queueAssetOp` extracted from the request callback, `opDisplayName`,
`opHeldBy`, `OurOp::display`), `CollabPanel.cpp` (entity rows, holder line, bundle headline),
`EditorApplication.cpp/.h` (`withoutPeerHeld(..., askHost)`, `onRemoteEntityOp` apply,
`AppContext::askHostToRename`, override unload on remote delete), `OutlinerPanel.cpp` /
`InspectorPanel.cpp` (rename routing; Details only for the scene world), `ContentBrowserPanel.cpp`
(Revert to Default), `HcRenameDialog.cpp` + `EditorHelp.cpp` ("Ask for it"). Tests:
`test_collab_controller.cpp` "Entity op keys…" and "Collab: deleting or renaming an entity a
peer holds goes to the host" (3 subcases).
**Verified:** out/build/release: `he_tests` (56 steps) and `HorizonEditor` (81 steps) build, rc 0.
New cases 10/10 green in a loop. Green: test_collab_controller 81 cases (one earlier run had a
`pumpUntil` flake outside the new cases; 4 reruns green), test_net_collab 113, test_collab_undo 9,
test_editor_commands 13, test_outliner_ui 9, test_collab_docsync 24, test_editor_help 19,
test_mcp_tools_collab 7. Full he_tests minus test_material_graph: 5126/5127; the one red is
test_hpak.cpp:1186 under the real HOME (54/54 with `HOME=$(mktemp -d)`, lesson 321).
`editor_help_audit.py --check`: 1167/1167.
**Not verified:** no live two-editor run. `EditorApplication.cpp`, `CollabPanel.cpp`, `HcRenameDialog.cpp`,
`ContentBrowserPanel.cpp` are not in he_tests: build-checked only.
**Decisions:** reuse the asset-op channel with a key prefix, no protocol bump; ask only on
conflict; host's own entity ask goes through its in-tray; HC sweep conflict = edit, so the
holder decides ("Ask for it" = `requestAssetEdit`), not the host.
**Pitfalls:** `CollabController.h` and `EditorApplication.h` edits rebuild most of he_tests.
Anything new that reads an asset-op `path` as a file must check `CollabEntityOps::isKey` first.
**Next steps:** 1. Step 7 (lost updates): re-send after a lock grant (MCP save, sweep writes on
a client). 2. Step 11: two editors, Bob selects a cube, Anna presses Delete / renames it,
approve and deny; Revert to Default in a session; Rename Across Project with a peer-held graph.
