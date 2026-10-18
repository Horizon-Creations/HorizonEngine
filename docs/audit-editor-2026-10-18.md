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

## Handoff

- **Topic / step:** 220, step 3 of 5 (steps 1 and 2: records above)
- **Branch / commits:** `claude/audit-optimierung-editor-stufe-6`: 17629338 (910/1802/1503/1404), e0e3c8a9 (302), 69ea22c0 (301), 1c4ac63f (909), then this record.
- **Done:** row clipping for the Content Browser grid, docs reader, source-control tree, UI widget hierarchy and asset picker; the palette without per-row allocations; outliner stage 1. Table in "Step 3" above.
- **Files touched:** `ContentBrowserPanel.cpp` (`tileInView`, before the folder loop), `DocsPanel.cpp/.h` (`drawPage`, `setSectionClipping`/`sectionsDrawn`), `SourceControlTree.h` (new), `SourceControlPanel.cpp` (`Model::tree`, `drawRows`), `UIEditorPanel.cpp/.h` (`HierarchyIndex`, `drawHierarchyNode`, hooks), `EditorWidgets.cpp` (picker), `HcEditorUtil.cpp` (`searchMenuItem`, `apiRowMatches`), `OutlinerPanel.cpp` (~1100); tests: `test_widget_designer_ui`, `test_ui_shot` (+2), `test_sourcecontrol_tree` (new, in `tests/CMakeLists.txt`).
- **Verified:**
  - Release `he_tests` + `HorizonEditor` build green. Full `ctest -j4`: 303/304. The red one is the known `test_hpak:1188` (6 == 2, EngineContentCache in this HOME).
  - All `ui shot*` frames from before are byte-equal after, except 8 that also vary run to run. The new clipped frames are pixel-equal to the unclipped ones.
  - Hidden-editor A/B for the grid (numbers above). `editor_help_audit.py --check` exits 0.
- **Not verified:**
  - The Source Control panel is not in `he_tests`. Its tree is only compiled, plus the pure-line tests; nobody has looked at it as an image.
  - A **scrolled** Content Browser A/B: there is no headless scroll, so only the top of the grid and the thumb were compared.
  - Windows/Linux (CI).
- **Decisions taken:**
  - Folder lines in the SC tree are now one frame high (`FramePadding`) like the file rows. The clipper needs one step, so they are a few px taller than before.
  - Folds of the SC tree are kept in the panel (they now survive a status refresh).
  - The palette has no clipper (see table); outliner stage 2 not done.
  - Picker row ids are the index, not `label##path`.
- **Suggested, not taken:** EDT-302's `splitWords` cache and per-page `panelForTopic` (now only per drawn section). EDT-304 (copy of the search hits per frame) is next in the grid's path. Outliner stage 2 after a 50k measurement.
- **Traps:**
  - Variable-height memos: trust only a height that was the same in two draws.
  - Editor A/B: the old binary must sit in the build's `HE_Editor` dir (EditorDeps), and the noise of two runs is ~70 px.
  - The widget designer test derives row ids from `Name##hn<id>`; keep that label.
- **Pending:** plan steps 4-5.
- **Next steps:** 1. Step 4: Collab EDT-402/1601/201/202 (signals and revisions instead of the per-frame structure diff).
