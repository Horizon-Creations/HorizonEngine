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

## Step 5: tables instead of parallel lists, first monolith split (EDT-801, 1208, CORE-603, M9)

| Finding | Where | What changed |
|---|---|---|
| EDT-801 asset panels | `EditorUI.cpp` `kAssetPanels` (before `tabHasUnsavedEdits`) | One `constexpr` row per panel (isAsset, render, isDirty, appendDirtyPaths, save, appendSnapshots, collabDocs, forget, reloadFromDisk); the eight functions and the render dispatch loop over it. Row order = dispatch order, the script editor is the last row (fallthrough, `isAsset == nullptr`). `static_assert`: every row renders and forgets, isDirty comes with appendDirtyPaths and save. Gaps kept as visible `nullptr`: Cinematic has no recovery copy, Skeletal Mesh no reload, the Static Mesh and Texture viewers only render/forget. The material "Advanced Shader Effects off" screen moved into the row's render function. |
| EDT-1208 MCP write gate | `McpToolCommon.h/.cpp` `refuseAssetWrite`, `WriteGateText` | Animator, clip, input, material, particle, sequence, type, widget: Engine → play → lock → dirty in one function; codes and messages byte-identical, play/dirty texts stay the family's own. Widget passes no dirty text (a dirty Designer tab takes the edit). Hook-struct base (`McpAssetGate`) not done: it lives in `McpToolRegistry.h`, which EDT-1209 splits. |
| CORE-603 UIPropTable | `UIElement.cpp` `baseProps()`, `findBaseProp` | One row per shared property (descriptor, getter, setter, `BaseOffer`); `uiBaseProperties`, `allProperties`, `getPropAny`, `setPropAny` cut from it; name index instead of up to 53 compares. **Two deliberate changes** (the drift): `allProperties` now offers Accepts Drop / Draggable / Drag Payload (visible in the MCP widget property lists; WidgetManager's rememberOriginal and the PreConstruct write record now see them too), and `uiBaseProperties` carries Gradient Angle 0-360 / Gradient Shape 0-1 (a bounded slider in the Theme editor and the widget parameter rows). |
| M9 `dumpFrameHeadless` | `EditorApplication.cpp` `buildDumpWitnessScenes` (before `dumpFrameHeadless`) | The 3157 lines of HE_DUMP_* witness scenes moved line for line into their own member; the material-test and layer-material ids are out-parameters. `dumpFrameHeadless` is ~1700 lines now. |

**Measured** (CORE-603 lookup, standalone `clang++ -O2`, M5, two runs): old chain vs index,
"Visible" (row 1) 20 → 40 ns, "Render Opacity" 85 → 40 ns, "Position" 205 → 33 ns, last row
400 → 42 ns, a type's own property ("Color", misses all 53 first) 400 → 62 ns.
EDT-801 claims no speed-up: the per-tab dirty check still asks every row, see Handoff.

**Verified:** EDT-801: HorizonEditor old vs new with Widget, Material, HorizonCode class,
Texture and C++ tabs forced active (temporary env hook, not committed): same panels and
labels, differences = caret/AA/FPS noise. EDT-1208: MCP tool tests 339 + `test_mcp_hc` 20,
new "refuseAssetWrite: Engine, play, lock, dirty, in that order, hooks asked lazily".
CORE-603: `test_ui_widgets` 428 incl. new "allProperties offers the shared rows exactly as
uiBaseProperties describes them", widget designer/MCP widget/theme tests. M9: Metal shots
byte-identical old vs new: `frame_pixel_ab.sh frame` (16), sky, material, SSR, water,
landscape layers, many lights, LAYEREDIT (sensitive: differs from layers alone) and the
preview stress loop (runs only if the material-test id came back).

## Step 6: MCP hooks out of EditorApplication.h (EDT-1209), CORE-603 re-checked

| Finding | Where | What changed |
|---|---|---|
| EDT-1209 header split | `McpToolRegistry.h` (179 lines: ToolResult, McpCallContext, McpTool, McpToolRegistry, `mcpBase64Encode`; json only), new `McpToolHooks.h` (the per-family hook structs, register functions, screenshot limits, `kMcpDoc*`; moved verbatim) | The 20 `McpTools*.cpp`, `McpToolRegistry.cpp`, `McpToolsApi.h` and 21 `test_mcp_*` include the hooks header; `McpBridge.h` and `McpToolCommon.h` keep the light one (`McpToolCommon.h` now names `Types/Enums.h` itself). |
| EDT-1209 ownership | `EditorApplication.h` (forward declaration after the includes, `std::unique_ptr<HE::Ed::McpBridge> m_mcp` in the old slot), `EditorApplication.cpp` (constructor out of line, `make_unique`) | Never null, never reset; same slot, so `m_mcpCameras` still outlives the bridge. `m_mcp.` → `m_mcp->` (38), `.mcp = m_mcp.get()`. `EditorSettingsPanel.cpp` and `McpStatusBar.cpp` include `McpBridge.h` for `AppContext::mcp`. |
| CORE-603 | `UIElement.cpp` `baseProps()` / `findBaseProp` (5ce314cd, step 5) | Nothing new: the table is the step 5 commit. Re-verified by `test_ui_widgets`. |

**Measured** (OutlinerPanel.cpp, Release flags, M5): before, `-ftime-trace` put the
`McpBridge.h` subtree at 177 ms of the 4958 ms `EditorApplication.h` parse. The audit's premise
does not hold: `json.hpp` (538 ms) and `IRenderer.h` reach every editor file anyway, through
`Application.h:9-11` → `GlobalState.h:11`. After: `-H` no longer lists `McpBridge.h`/
`McpToolRegistry.h`. The preprocessed TU loses 391 of 152 758 lines, and user CPU is within
noise (old 12.75-13.36 s, new 12.66-13.02 s, alternated, load 10-12). The win is the rebuild
fan-out, from `ninja -t deps` (old = today's users of the two headers plus every
`EditorApplication.h` user): editing the hooks now recompiles **23 editor + 44 test objects
instead of 101 + 94**, editing `McpBridge.h` **4 + 3 instead of 78 + 52**.

**Verified:** `HorizonEditor` and `he_tests` build green (first try red on the two includes
named above). `test_mcp_*` 412/412, `test_ui_widgets` 428/428, widget designer + theme 33/33.
Full `he_tests` without `test_material_graph`: 5348/5351, the same three as step 5
(`test_hpak:1188`, `test_profiler` 107/215, profiler green alone). `editor_help_audit.py
--check` green.

## Handoff

- **Topic / step:** 220, step 6 (steps 1-5: records above)
- **Branch / commits:** `claude/audit-optimierung-editor-stufe-6`: 1023b16a (EDT-1209), then this record.
- **Done:** table above. CORE-603 was already complete in 5ce314cd; this step only re-ran its tests.
- **Verified:** see "Verified" above. Windows/Linux only through CI.
- **Not verified:** no live MCP client was connected to the editor (the hidden run `HE_HIDDEN_WINDOW=1 HE_EXIT_AFTER_FRAMES=120 HE_MCP=1`, private HOME, logged "MCP: listening on 127.0.0.1:…" at frame 1 and "listener stopped" at shutdown, rc 0, endpoint file gone; the socket handshake itself is `test_mcp_bridge`). MSVC: only CI sees the `unique_ptr` of a forward-declared class with the out-of-line ctor/dtor.
- **Decisions taken:** the light header keeps the name `McpToolRegistry.h` (its class), the hooks get the new name, so `McpBridge.h` needed no change. Prose blocks in the headers stay (the audit's item 3; the code base keeps its why-comments there). No `McpAssetGate` base: it would move three fields out of eight hook structs, and with a base class those structs stop being aggregates, which breaks their designated initialisation.
- **Not done:** EDT-703/704 (product question still open on the topic), `McpAssetGate` (above), CORE-603 follow-ups from the audit (`allProperties()` copies ~50 descriptors per call in `UIEditorPanel.cpp` per frame; `applyClipAt` resolves each track by name per frame, a slot could be cached at clip start), M9 for `ContentBrowserPanel::render` and `EditorUI::renderEditor` (step 7), Cinematic recovery copy (EDT-801).
- **Traps:**
  - The real `json.hpp` source for every editor TU is `Diagnostics/GlobalState.h:11`, pulled in by `Application/Application.h`. A json-free editor header chain starts there, not in the editor.
  - Compile timing under other bees' builds: wall time swung 11-37 s for the same TU. Alternate A/B and compare user CPU or preprocessed size, or count objects in `ninja -t deps`.
  - `restoreOpenTabs` / witness traps from step 5 still apply.
- **Next steps:** 1. Step 7: M9 `ContentBrowserPanel::render`, then `renderEditor` (UI-shot A/B; ~70 px noise in the Content Browser region). 2. EDT-703/704 once the question is answered. 3. Optionally, survey first: GlobalState.h without json would take json.hpp out of every editor TU, but it also exports a global `using json = nlohmann::json;` (`GlobalState.h:13`) that other files may lean on, so count those before promising anything.
