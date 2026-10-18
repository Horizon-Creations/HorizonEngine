# Audit-Optimierung: Editor (Stufe 6)

Topic 220, from `docs/audit/AUDIT.md` Stufe 6 and `docs/audit/modul_editor.md`.

## Step 1: one asset file index instead of a tree walk per dropdown and frame

`HcEditorUtil::listAssets` used to walk both content roots and open every `.hasset` header
on each call, and draw code called it from seven places every frame (EDT-901, 902, 1001,
1002, 1101, 1702, 1801). Two layers fix this:

**The index (`ContentManager::assetFiles(type)`, `sceneFiles()`).** One header-only walk
sorts every `.hasset` of the content root, then the engine root, into its type. The result
stands until `contentEpoch()` moves or a root changes. The walk order and the roots are the
old `listAssets` ones. The SFTP cache dir is *not* included: `scanContentDirectory` walks
it, but pickers never listed it. Scenes get the same treatment (project root, sorted).

Results come back as `shared_ptr` snapshots, so a held list survives a rebuild and any
`loadAsset`. The index is main-thread only and has no lock, like `EditorAssetTypeCache`.

**Invalidation.** It rides `contentEpoch`. That was already bumped by `saveAsset` and
`noteContentChanged`; `scanContentDirectory()` now bumps it too. The editor runs that scan
after every refresh of its folder trees (modal, quiet create/rename, the periodic
external-change poll, the SFTP re-merge; see `EditorUI.cpp` ~775). So the pickers are
exactly as fresh as the Content Browser. The only other epoch reader,
`RenderExtractor::setContentManager`, merely forgets its "material missing" set once more
per refresh.

Batch jobs that must see everything call `noteContentChanged()` first: the rename sweep
(`HcRenameSweep.cpp` `classAndDescendants`, `scan`) and the export's codegen collection
(`ExportDialogPanel.cpp` ~1362).

**Per call site:**

| Finding | Where | Change |
|---|---|---|
| EDT-901/1002 | `HcEditorUtil.cpp` `drawPinDefaultEditor`, `engineParamChoices`; `HcGraphHost.cpp` ~896 | new `engineParamHasChoices` + optional `hasChoices` callback; the list is built only while the combo is open. One classifier `paramSourceOf` feeds both, so has == !list.empty() by construction. The `save.*` schema is parsed once per template text (keyed on the text). |
| EDT-902 | `drawTypePicker` | enums/structs/objects taken once on `IsWindowAppearing`; objects are the raw class+widget refs, so `listClasses` (load + JSON parse of every class) is gone from the picker |
| EDT-1001 | `InputAssetPanel.cpp` ~745 | `listAssets` inside `BeginCombo` |
| EDT-1101 (2) | `LevelScriptPanel.cpp` base-class popup | class list + `wouldCycle` (ancestor chains) taken once on open |
| EDT-1101 (3) | `MaterialEditorPanel.cpp` "Set Texture" | texture list taken once on submenu open |
| EDT-1101 (1) | Inspector script-class combo | **already gone**: removed in a9770bfc, no `HcEditorUtil` call left in `InspectorPanel.cpp` |
| EDT-1702 | `TypeAssetPanel.cpp` `typeNamePicker` | `listAssets` inside `BeginCombo` |
| EDT-1801 | `UIEditorPanel.cpp` palette ~7378 | reads `assetFiles(Widget)` in place: no walk and no copy per frame |
| CORE-202 | `discoverAssets` | already done in Stufe 1 (#166, `docs/audit-core-fundamente-2026-10-17.md`) |

**Measured** (temporary doctest case, removed again): synthetic project with 3001 class
assets and 2000 other files, load average ~4-7 from other bees.

| Call | Time |
|---|---|
| One walk with header reads (the old cost of *every* `listAssets` call) | 660 ms |
| Cached `listAssets` (vector copy of 3001 entries) | 0.06 ms |
| `assetFiles()` snapshot | 0.00004 ms |
| `engineParamHasChoices` | 0.0002 ms |

The 660 ms is inflated by the load. The ratio is the point.

## Step 2: entityByUuid through the id index, a member set in EditorSelection

**`HE::Ed::entityByUuid` (EDT-1201, 1602, 503).** It walked every `EntityIdComponent`,
formatted each id with `snprintf` into a `std::string` and compared text, with no early
exit. Now the string form parses the 32 hex digits once and asks
`HorizonWorld::findByEntityId`, the O(1) index the registry's signals feed. A new
`entityByUuid(world, const HE::UUID&)` overload skips the text. The task board's
orphan check (every second, per entity link), its jump and its pending select used to
format the id with `uuidToHex` and parse it straight back. They now pass the `UUID`
directly (`TaskBoardPanel.cpp` ~547, 550, 579, 603). The MCP callers (`McpToolsEntity`,
`McpToolsApi` via the `EditorApplication` hook, prefab, terrain) keep the string form.

Behaviour kept on purpose:
- Lowercase only. The parser in `EditorCommands.cpp` is `uuidFromHex` (`TaskBoard.h`)
  minus upper case, because upper case never equalled `uuidOf`'s text. Do not "dedup" it
  into accepting upper case without deciding that.
- The zero id never matches; `findByEntityId` treats it as "no identity". The old scan
  could match an entity whose id was zero; `createEntity` never makes one.
- With two holders of one id (a damaged merge) you get one of them, not "the first in
  view order".

The audit's second idea for EDT-1602 (one world pass per orphan refresh) is not needed:
links × O(1) is already less than one pass.

**`EditorSelection` (EDT-701, 1805).** It now keeps an `unordered_set<Entity>` beside the
ordered vector, maintained by every mutator. `contains()` is O(1), so `roots()` is
O(S·depth), and `addMany`/`setMany` are O(n). Order, primary, anchor, the revision bumps
(including `setMany`'s double one) and the `kMaxDepth` cycle cap are unchanged. Every
`contains()` caller benefits too: the outliner (per visible row) and
`ViewportOverlays::appendSplineGuides` (per spline). The viewport still calls `roots()`
every frame. Caching it per selection revision was **not** done: a reparent does not
move the revision, and at 1.25 ms for 20k members it is no longer the hitch.

**Measured** (standalone, built with the engine's own compile flags: old files from
HEAD against new, `-O2`, M5 MacBook Air, mains power, low-power mode off, `pgrep yes` = 0,
the `he_tests` build ran in parallel):

| Call | old | new |
|---|---|---|
| `roots()`, Select All of 5 000 (chains of 4) | 10.3 ms | 0.28 ms |
| `roots()`, Select All of 20 000 | 162 ms | 1.25 ms |
| `setMany`, 20 000 | 225 ms | 3.5 ms |
| one `entityByUuid`, 5 000 entities | 1.5 ms | ~µs (index find) |
| one `entityByUuid`, 50 000 entities | 15 ms | ~µs |

The `entityByUuid` row compares the old loop with the same `unordered_multimap` lookup
(the "new" column includes a `substr`/`strtoull` parse the real code does not allocate).

**Tests:** `test_editor_selection.cpp` gained "roots over a Select All of thousands,
chains and a cycle" (5 000 members, a parent cycle above a member) and "contains agrees
with entities after every change". `test_editor_commands.cpp` gained "entityByUuid follows
the world" (same-frame create, `setEntityId`, a hand-emplaced component, destroy), the
strictness checks (length, non-hex, upper case, UUID overload), and a resolve after a
**snapshot** undo/redo. That is the path the old header comment named as the reason not
to use an index.

## Step 3: row clipping where it was missing (M12)

One scheme where rows are nested or heights differ, the Outliner's: a row (or section)
outside the visible band only adds its measured height to a run that is laid out as
**one `Dummy`** before the next drawn row and after the last, so the scroll range is
what drawing everything gives. Heights are measured off drawn rows; until one is known,
everything is drawn. Flat lists of one-line rows use `ImGuiListClipper` directly.

| Finding | Where | What changed |
|---|---|---|
| EDT-301 Content Browser grid | `ContentBrowserPanel.cpp`, `tileInView` before the folder loop | A row of tiles (folder and file alike) is one height, measured off two drawn rows; rows out of view are skipped whole. Everything is drawn while a drag payload exists or `s_revealScrollPath` waits. |
| EDT-302 Docs reader | `DocsPanel.cpp` `drawPage` | Per-section heights; only a height that came out the same in two draws is trusted (a table settles its columns over a frame: without that rule `docs-examples` came out 1 px off). Reset per page, body width, font size and `s_lang`; no skipping while `s_scrollFrames > 0`. |
| EDT-1503 Source control tree | `SourceControlTree.h` (new), `SourceControlPanel.cpp` `drawRows` | The tree is built once per status snapshot in `rebuildModel` as flat lines (folder/file, depth, subtree end, folded chain label) and drawn through the same clipper as the flat view. Folds live in `s_foldedDirs[section]` (by full path) instead of ImGui storage. |
| EDT-1802 UI widget hierarchy | `UIEditorPanel.cpp` `HierarchyIndex`, `drawHierarchyNode` | One children index per frame instead of `childrenOf` per row (n²); rows out of view skipped the Outliner way under the same `Name##hn<id>` ids (the designer test derives ids from that label); label and popup id without allocation. After a drop or Delete in the walk the index is stale, and the rest of that frame asks the tree, as before. No skipping during a drag. |
| EDT-910 Asset picker | `EditorWidgets.cpp` `assetSlot` popup | Filter result kept as indices, rebuilt on query change only; `ImGuiListClipper`; row id = index (`PushID`) instead of `label##path`. |
| EDT-909 Node palette | `HcEditorUtil.cpp` `searchMenuItem`, `apiRowMatches`, `drawEngineApiMenu` | **No clipper**: ranking and the arrow keys need every row registered, and ImGui's `ItemAdd` already culls rows it cannot see. Allocations per row removed (one lowering buffer, key list reuses last frame's strings, in-place case-insensitive match). |
| EDT-1404 Outliner | `OutlinerPanel.cpp` ~1100 | Stage 1 only: `visibleRows` is static and filled only while Shift is held. Stage 2 (visible list without the O(N) walk) is open; the audit wants a measurement first. |

**Measured** (hidden editor, Metal, M5 MacBook Air, no `yes` load, other bees' builds may
have run): tutorial project plus 3 000 files in `Content`, Content Browser open on that
folder. Old binary **182 FPS**, new **505 to 523 FPS** (two runs), FPS read off the
status bar of the `HE_DUMP_UI` frame. The grid region of old and new differs by 22 px,
inside the 72 px that two runs of the same binary differ by; the scroll thumb is identical.

**Frame budget in the UI tests:** asserted as rows submitted, not wall-clock (this Mac
flakes under build load): the hierarchy submits 10 to 120 of 364 rows, the docs page
fewer than 12 of 120 sections. Both tests also compare the clipped frame pixel for pixel
with the unclipped one (`setHierarchyRowClipping`, `DocsPanel::setSectionClipping`) and
check the scroll range.

**Tests:** `test_widget_designer_ui.cpp` "the hierarchy clips rows out of view" (404
elements, one folded box, scrolled to the middle); `test_ui_shot.cpp` "the
documentation reader skips sections out of view" (120 sections, plus a jump) and
"search palette: typing ranks, the arrow walks, Enter picks" (characterisation, green
against the old code too); `test_sourcecontrol_tree.cpp` (new: order, chain folding,
folds, 5 000 files). All `ui shot*` frames from before the step are byte-equal after it,
except the 7 console shots and `texture_viewer_asset`, which differ between two runs of
the same build as well.

## Step 4: collab structure diff, net ids and doc diffs on signals (EDT-402/1601/202/201)

| Finding | Where | What changed |
|---|---|---|
| EDT-402/1601 structure diff | `StructuralSync.h` `StructPassGate`; `EditorApplication.cpp` `syncStructuralChanges(nowMs)` | The pass runs when `HorizonWorld::structureEpoch()` moved (create, destroy, reparent, sibling order, clear, loaders, cell streaming), while a create/re-parent/destroy waits for a retry (`m_structureUnsendable`, `m_structureUndeletable`, new `m_structureReparentPending`: `kSendRetryFrames` still counts frames), or once a second (fallback for raw `registry().create()`, hand-rewired hierarchy, a component that makes an entity local-only). Gate reset outside a session and in `onWorldReplaced`. `current` is a member set. The diff rules are unchanged. |
| EDT-202 net-id table | `CollabController.h/.cpp` `m_netByHandle`/`m_handleByNet`/`m_netMisses`, `rememberNetId`, `netIdOf` | Two hash maps instead of the vector of pairs; first entry per key wins, as the first match did. `seedNetIds` reserves and is O(N). A miss of `entityForNetId` is cached against the new `HorizonWorld::entityIdRevision()` (bumped by the id signals that already feed `findByEntityId`), so a peer's selection of an entity this machine lacks scans the registry once per id change instead of every frame. Misses capped at 4096; cleared on `setWorld` to another world, session end, seed. |
| EDT-201 Level Script / Game Instance diff | `CollabDocSync.h` `DiffGate`; `EditorApplication.cpp` `editorDocDiffDue`, the tab and untabbed loops of `updateAssetCollabSync` | Diff (and `hasLocalChanges`) when `m_undo.revision() + m_editorDocEdits` moved (Level Script edits snapshot; `commitGameInstance` and `hc.endEdit` bump the count), the lock changed hands or the mirror is unseeded; otherwise every 250 ms. Gate at the per-frame callers only: the MCP `beginEdit` path calls `publishDocDeltas` directly and always runs. A skipped untabbed key stays in `openRel` (keeps its lock). |

**Measured** (standalone `clang++ -O2` against the real `StructuralSync.h` and entt, M5,
flat scene under the root, per frame averaged over 30 frames; two runs, at load ~10 from
parallel builds and at load ~3.7):

| Entities | old diff per frame | gate, idle frame | seed: vector | seed: maps |
|---|---|---|---|---|
| 1 000 | 0.053 / 0.055 ms | ~1 ns | 0.2 / 0.6 ms | 0.05 / 0.06 ms |
| 10 000 | 0.83 / 0.48 ms | ~1 ns | 52 / 19 ms | 0.65 / 0.46 ms |
| 50 000 | 6.3 / 2.3 ms | ~1 ns | 1 114 / 488 ms | 4.9 / 2.2 ms |
| 100 000 | 9.7 / 4.7 ms | ~1 ns | (not run) | 12 / 4.0 ms |

The idle-session cost of the structure pass is now the fallback pass once a second
(so ~1/60 of the old per-frame cost at 60 FPS). The doc gate was not timed: the audit's
own estimate is 0.1-2 ms per frame for a few-hundred-node graph, now at most 4 diffs a
second plus one per edit.

**Tests:** `test_collab_controller.cpp`: "Net ids: a seeded world resolves every entity
both ways" (20 000), "...an id nobody has is not found until an entity takes it",
"...a cached miss also yields to a raw emplace of the id", "...forgetting an id drops it
both ways, and a recycled handle gets its own"; "Structure gate: ..." (idle frames do not
run, each structure change wakes it, retries and the fallback). Making a cached miss
ignore the revision turns two of the net-id tests red. `test_collab_docsync.cpp`: the
`DiffGate` rules, and an unsignalled edit on a real HorizonCode graph sent by the
fallback diff.

## Handoff

- **Topic / step:** 220, step 4 of 5 (steps 1-3: records above)
- **Branch / commits:** `claude/audit-optimierung-editor-stufe-6`: 70440211 (code + tests), 93f96ef0 (this record), then the retry-map fix (the commit after it).
- **Done:** EDT-402/1601 (structure pass gated), EDT-202 (net-id maps + miss cache), EDT-201 (Level Script / Game Instance diff gate). Table in "Step 4" above.
- **Files touched:** `StructuralSync.h` (`StructPassGate`), `EditorApplication.h/.cpp` (`syncStructuralChanges(nowMs)` ~11570, caller ~4990, `onWorldReplaced` ~1058, `editorDocDiffDue` before `publishDocDeltas`, the two loops of `updateAssetCollabSync`, `hc.endEdit`, `commitGameInstance`), `CollabController.h/.cpp` (net-id section ~2700, remote create ~1163, session reset ~1768, `setWorld`), `CollabDocSync.h` (`DiffGate`), `HorizonWorld.h/.cpp` (`entityIdRevision`); tests: `test_collab_controller.cpp` (+7), `test_collab_docsync.cpp` (+2).
- **Verified:**
  - Release `HorizonEditor` and `he_tests` build green. Full `ctest -j4`: 301 passed, 2 skipped, 1 failed: the known `test_hpak:1188` (EngineContentCache in this HOME).
  - Collab files alone: controller 97, docsync 26, mcp_tools_prefab 22, mcp_tools_collab 7, net_collab 114, collab_undo 9, all green. Miss-cache mutation turns 2 new tests red.
  - Standalone bench (table above).
- **Not verified:**
  - No live two-editor session was run. The gating inside `syncStructuralChanges`/`updateAssetCollabSync` is only compiled: `EditorApplication` is not in `he_tests`. Only the gate rules and the net-id table are tested.
  - The doc-gate saving was not timed. Windows/Linux only via CI.
- **Decisions taken:**
  - A structure epoch plus a 1 s fallback, not entt `on_construct` sinks. The epoch already covers the world's own paths, and sinks would need re-wiring on every world replace.
  - The EDT-201 signal is a sum of counters that only grow, plus a 250 ms fallback. That was the audit's lower-risk variant; there is no revision on `HorizonCode::Graph`.
  - `forgetNetId` drops one handle entry (the one under that id). A second handle that cached the same id (a damaged merge) keeps its entry, where the vector dropped every pair.
- **Suggested, not taken:** the audit's randomised equivalence test (full diff vs gated pass over create/destroy/reparent/undo) needs the pass moved out of `EditorApplication` into a testable `StructuralSyncPass`.
- **Traps:**
  - `onWorldReplaced` marks EVERY entity known, local-only ones included. The first pass then sends destroys for this machine's terrain chunks and lights, which receivers ignore. Found by reading the code, not tested. This was there before the step and was left as it is.
  - A cached net-id miss is only valid for one world: `setWorld` clears it.
  - Anything left in the retry maps (`m_structureUnsendable`, `m_structureUndeletable`, `m_structureReparentPending`) forces the pass on every frame. So dead entities are pruned from `m_structureUnsendable` at the start of each pass, both maps are cleared at session end and in `onWorldReplaced`, and a re-parent under a parent that is local-only is not marked pending (that parent will never be known).
- **Pending:** plan step 5.
- **Next steps:** 1. Step 5: tables instead of parallel lists (EDT-801, 1208/1209, 703/704, AssetPanelOps/UIPropTable), then Extract Method on the big monoliths.
