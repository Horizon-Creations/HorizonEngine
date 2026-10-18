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

## Handoff

- **Topic / step:** 220, step 1 of 5
- **Branch / commit:** `claude/audit-optimierung-editor-stufe-6` @ 0b702211 + this doc (pushed: see the Hive post)
- **Done:** every picker/palette call of `listAssets`/`listScenes` reads one epoch-keyed index; the dropdowns fetch only when open.
- **Files touched:** `ContentManager.h` (~92-130 API, ~905 `FileIndex`), `ContentManager.cpp` (after `scanContentDirectory`), `HcEditorUtil.cpp/.h`, `HcGraphHost.cpp`, the five panels above, `HcRenameSweep.cpp`, `ExportDialogPanel.cpp`.
- **Verified:**
  - Release build of `he_tests` + `HorizonEditor` is green; all edited panels compiled.
  - Targeted tests are green: `test_contentmanager` (91, incl. 2 new), `test_horizoncode_shortcuts` (6, incl. new has==list), `test_inspector_ui`, `test_engine_weather_sounds`, `test_hc_rename`, `test_culling`, `test_ui_shot`.
  - Full `he_tests`: 5431/5434. The 3 red are the `test_hpak:1188` case and two `test_profiler` cases.
  - `test_hpak:1188` counts the SFTP cache in `$HOME`; it is green with an empty HOME and pre-existing.
  - `test_profiler` was red under the parallel editor build and is 18/18 three times alone.
  - `editor_help_audit.py --check` reports 1205/1205.
- **Not verified:** no manual editor session (combo open/close, palette after creating a widget); no Windows/Linux build (CI).
- **Decisions taken:**
  - The epoch is the revision, not a second counter.
  - The type picker's Objects list now includes widgets/classes whose graph is empty. listClasses dropped them, but a widget without logic is still a valid object type.
  - `listClasses` stays (public, now unused by the picker).
- **Suggested, not taken:**
  - `discoverAssets` (PlayerHost calls it 3x) could use the index too. It is not done because game/tests write files without bumping the epoch.
  - The `test_hpak:1188` HOME dependency deserves its own fix (point `engineContentCacheDir` at a temp dir in the test).
- **Pending:** steps 2-5 of the plan.
- **Traps:**
  - A tool that writes `.hasset` files without `saveAsset` and without a content refresh is invisible to pickers until the next refresh/poll. Call `noteContentChanged()` there.
  - `drawTypePicker`/LevelScript/Material snapshots are function statics keyed on popup open (ImGui marks a popup "appearing" again when its id changes).
- **Next steps:** 1. Step 2: `entityByUuid` → `HorizonWorld::findByEntityId` (EDT-1201, 1602, 503), `EditorSelection::roots()` with a set (EDT-701, 1805).
