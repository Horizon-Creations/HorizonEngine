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

## Handoff

- **Topic / step:** 220, step 2 of 5 (step 1: 0b702211, its record above)
- **Branch / commit:** `claude/audit-optimierung-editor-stufe-6`, the step-2 commit after 004a8530 (hash in the Hive post)
- **Done:** `entityByUuid` answers from `findByEntityId` (+ `UUID` overload, used by the task board); `EditorSelection` keeps a member set, `contains` O(1), `roots` O(S·depth).
- **Files touched:** `EditorCommands.h` (~312-325), `EditorCommands.cpp` (`entityByUuid`, end of file), `EditorSelection.h/.cpp`, `TaskBoardPanel.cpp` (547, 550, 579, 603), `tests/test_editor_selection.cpp` (+2 cases), `tests/test_editor_commands.cpp` (+1 case, +checks in two).
- **Verified:**
  - Release `he_tests` + `HorizonEditor` are green; `EditorApplication`, `TaskBoardPanel`, `EditorCommands` and `EditorSelection` all compiled.
  - Targeted tests are green: `test_editor_selection`, `test_editor_commands`, and `test_mcp_tools_{entity,scene,prefab,terrain,taskboard}`, `test_mcp_api`.
  - Full `ctest -j4`: 302/303. The red one is `test_hpak:1188` (6 == 2): 4 `.hasset` in this Mac's EngineContentCache. It is 59/59 with an empty HOME and pre-existing.
  - `editor_help_audit.py --check` exits 0; no new controls.
- **Not verified:** no manual editor session (Ctrl+A in a big scene, task-board jump); no Windows/Linux (CI).
- **Decisions taken:** lowercase-only parse (old string-compare semantics); zero id never matches; no per-revision `roots()` cache in the viewport (a reparent does not bump the revision); `setMany`'s double revision bump left as it was; no single-pass orphan check (O(1) lookups make it moot).
- **Suggested, not taken:**
  - EDT-702 (group gizmo rebuilds `worldMatrixOf` per member per frame) is the next cost in the same viewport path after `roots()`.
  - Step 1's open points stand: `discoverAssets` on the index, the `test_hpak` HOME fix, and folding the index walk into `scanDirInto`.
- **Traps:**
  - `findByEntityId` cannot see `registry.get<EntityIdComponent>(e).id = x` (no signal). A grep of `src/` found no such write; write through `setEntityId`/`patch`.
  - Step 1's still apply: write `.hasset` without `saveAsset` → `noteContentChanged()`; pickers snapshot on popup open.
- **Pending:** plan steps 3-5.
- **Next steps:** 1. Step 3: row clipping (M12) where it is missing: EDT-301, 302, 1503, 1802, 909, 910, 1404; check the UI-test frame-time budget.
