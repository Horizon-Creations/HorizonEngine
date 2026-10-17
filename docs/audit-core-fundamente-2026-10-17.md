# Audit-Optimierung: Core-Fundamente (Stufe 1)

Thema 213, from `docs/audit/AUDIT.md` Stufe 1 and `docs/audit/modul_core.md`.

## Step 1: CORE-201, CORE-202, CORE-204 (ContentManager)

**CORE-201.** `ContentManager.h` now holds the one list of all 26 pools, next to the
members: `anyPool(f)` short-circuits, and `forEachPool(f)` is built on it.
`unloadAsset`, `forgetProjectContent` and `retargetAssetReferencesInMemory` walk that
list instead of their own hand-kept copies. Those copies had drifted:

- `unloadAsset` was missing struct, enum, save-template and theme.
- A project switch kept the theme, bone-mask and blend-space pools.
- A rename left six types with their old `path`.

There is no `static_assert` on the pool count, because it would only be a second hand
count. The test is the drift guard. Turning the `insert` sites and `register*` into a
`pool<T>()` trait is CORE-214 and was left alone.

**CORE-202.** `discoverAssets` uses `HAsset::readAssetTypeFromFile`, which reads the
32-byte header, instead of `Reader::open`, which reads every chunk.
`sniffAssetTypeFromFile` now delegates to it as well. This is option (a) from the audit.
Option (b), the registry, would miss files added after the last scan.

**CORE-204.** These parts make up the fix:

- **`saveAsset`** writes the mtime of `resolveAbsolutePath(path)`, which is the file the
  poll stats. It does this only for paths that are already in `m_pathMtime`.
- **`pollHotReload`:** if `unloadAsset` refuses (the asset is pinned), the poll records
  the mtime, logs once, and does not report the asset.
- **Survey of every `saveAsset` caller in `HE_Editor`.** Two of them depended on the
  self-reload, and both now do that work themselves:
  - `MaterialEditorPanel::saveToDisk` calls `regenerateMaterialFromGraph` and
    `syncMaterialInstancesOf` before the write. Without that, the GI `approx*` colours
    were only recomputed by the load.
  - `commitSlots` in the static and skeletal mesh tabs calls `InvalidateMesh`. The GPU
    copy holds the old material's texture, and the tab preview draws it.
  - Script hot-patching is unaffected: `ScriptEditorPanel` writes without `saveAsset`.

## Handoff (step 1)

- **Topic / step:** 213, step 1 of 4
- **Branch / commit:** `claude/audit-optimierung-core-fundamente-stufe-1`, commits
  02cd9a57 (201), 5f33f575 (202) and 2650d40e (204), plus this doc.
- **Files touched:**
  - `ContentManager.h`: `anyPool`/`forEachPool` after the pool members.
  - `ContentManager.cpp`: `sniffAssetTypeFromFile`, `saveAsset` (mtime),
    `unloadAsset`, `discoverAssets`, `pollHotReload`,
    `retargetAssetReferencesInMemory`, `forgetProjectContent`.
  - `MaterialEditorPanel.cpp` `saveToDisk`.
  - `StaticMeshEditorPanel.cpp` / `SkeletalMeshEditorPanel.cpp` `commitSlots`.
  - `tests/test_contentmanager.cpp`: three new cases.
- **Verified** (Windows, MSVC Release, `C:/hw213`):
  - `test_contentmanager` passes 89/89.
  - Negative control: the old `ContentManager.cpp` against the new tests fails exactly
    the 3 new cases (17 assertions: 6 stale paths + 4 refused unloads + 4 still
    loaded, 1 self-save, 2 pinned). The 201 commit message says "10 checks"; it means
    the 10 stale-path and refused-unload checks.
  - `test_player_host`, `test_project_exporter` and `test_asset_ref_retarget` are green.
  - `HorizonEditor` builds.
  - Full `he_tests` run: 5416 cases, 4 failed.
    - 2 are the `inspector_ui` clipboard cases, which are known baseline.
    - 2 are `EngineProfiler` cases that depend on run order: `test_profiler.cpp` passes
      alone (18/18). Not in the baseline note and not compared against the base commit;
      the CI run will show whether they reproduce.
  - `saveAsset` callers outside `HE_Editor`: the importers (`ImporterCommon.cpp`
    310/358) and the `*_gen` tools save through their own temporary `ContentManager`,
    so the editor's poll still sees a reimport. `HE_Scene` and `HE_Python` have none.
    No prefab tab saves through `saveAsset`; `pushToPrefab` syncs placements itself.
- **Not verified:**
  - The editor was not run by hand: GI colours after a Material Editor save, and the
    mesh-tab preview after a slot change.
  - The forget-pools leak is not observable through the public API (the handle maps
    were already cleared), so no test covers it. The single list is the fix.
  - No timing of `discoverAssets` on a large project.
- **Decisions taken:**
  - The Material Editor regenerates via the manager at save time. This reproduces exactly
    the state the self-reload used to produce. A codegen still runs once per save.
  - Mesh invalidation runs on every slot edit, even when the save fails, because the
    in-memory asset changed either way.
- **Decisions suggested, not taken:**
  - Compute `approx*` live in the editor's `regenerateMaterial`, which would mean
    exposing `applyApproxSurface`. GI colours would then be right before a save too, and
    the second codegen at save would go away.
  - `EditorApplication.cpp` ~3477 (`forgetFunctionGraph` on reload) is redundant for the
    editor's own saves (MEP 206 already does it). Harmless; left in.
- **Traps:**
  - `tests/HorizonCore.dll` goes stale after a Core-only edit. Copy it from
    `src/HE_Core`.
  - A source file restored with `Copy-Item` keeps its old mtime, so ninja does not
    rebuild it (lesson 376).
  - Struct/enum test JSON must stay unparsable, or the definitions land in the global
    `TypeRegistry`.
- **Next steps:**
  1. Step 2: UIWidgetTree id→index plus child index, then one layout pass
     (CORE-601/602/701-703).
  2. Optionally the `approx*`-in-editor follow-up above (an Editor/Material topic).

## Step 2: one layout pass per widget and frame (M2, CORE-601/602/701-703)

The widget layout answered every question ("where is this element", "is it on screen")
by walking the parent chain. Each step of that walk was a linear id lookup, a box child
rescanned the whole tree twice, and a Tab Box counted through every element to find
its page. Asked for every element every frame, that was O(n²) and worse.

**Design: a frame-scoped `UILayoutScope`, not a persistent index.**
`UIWidgetTree::elements` is public and written directly in many places (graft,
designer, MCP tools), so a cache on the tree would need invalidation that nobody can
enforce. The scope is RAII, registered thread-locally per tree, and has two lifetimes:

- **Structure:** id → element, parent → children in tree order, and each element's
  place among its siblings. It holds until an element is added, removed or moved.
  `extract` opens it after `syncLists`.
- **Values** (`memoizeValues()`): rect, unbounded rect, unit scale, visible, enabled,
  opacity, and each grid's solve. They hold only while nothing is written: the draw
  loop and the `topmostHit`/`windowHitAt` scans.
- **Auto-size borrows the value memo.** A generation counter forgets every remembered
  value at once whenever a size write changes something. Sizes are the only layout
  input that auto-size and the `applyAutoSize` overrides write.

The public functions keep their signatures and arithmetic: they ask whether a scope is
active and otherwise walk. Outside a scope (designer, MCP tools, single-question tests)
nothing changes.

The other changes in this step:
- **Box and wrap box:** they write every visible child's slot during the one walk that
  a child's question already makes.
- **Grid:** `cellOf` is a table built once per solve. Before, it scanned every cell for
  each child and each auto track, which for 150 children was about a million lookups.
- **Hit test (CORE-702):** `topmostHit` checks `hitTestable` before the inherited
  states. This is a conjunction, so the result is the same.
- **CORE-602:** `UIText::applyAutoSize` caches its measurement together with all of
  its inputs, and still writes the size every time.

**Measurement.** Bench: `test_ui_layout_scope.cpp`, run with `--no-skip`. An 1873-element
shell (Splitter, Tab Box, a scroll box with 400 rows, a grid, a wrap box, an accordion),
Release build on NN-WS03, through the real `WidgetManager`:

| | extract ms/frame | processPointer ms/call |
|---|---|---|
| before (9a8e81de) | 44.5 | 7.0 |
| scope (fa85c46a) | 6.2 | 0.38 |
| + borrowed auto-size memo (1ff11c3f, with the text cache) | 0.77 | 0.37 |

All three rows emit the same 2847 quads. After the scope, auto-size was 5.8 of the
6.2 ms; the borrowed memo is what removed it. The text cache was measured only before
that (no visible change at 6.2 ms, short labels); its share at 0.77 ms is not measured.

## Handoff (step 2)

- **Topic / step:** 213, step 2 of 4
- **Branch / commit:** `claude/audit-optimierung-core-fundamente-stufe-1`. Commits:
  9a8e81de (bench, before), fa85c46a (scope), 1ff11c3f (auto-size memo and text cache),
  plus this doc.
- **Files touched:**
  - `include/UIWidget/UILayoutScope.h`: new header; the rules are in its comment.
  - `UIWidgetTree.cpp`:
    - the hooks at the top: `find`, `forEachChild`, `memoFor`;
    - the walks `boxSlotRect`, `solveGrid`/`GridSolved::indexCells` and
      `wrapSlotRect`;
    - `struct UILayoutScope::Impl` and `BorrowedMemo`, before `uiElementUnitScale`;
    - the memo in `uiElementRect`/`Unbounded`, `Visible`/`Enabled`/`Opacity` and
      `uiChildIndexOf`/`CountOf`;
    - the borrowed memo in `uiApplyAutoSize`.
  - `WidgetManager.cpp`: scopes in `extract`, `topmostHit` and `windowHitAt`.
  - `UIElements.h` / `UIElement.cpp`: `UIText::MeasuredFor` and `applyAutoSize`.
  - `tests/test_ui_layout_scope.cpp` (new).
- **Verified** (Windows, MSVC Release, `C:/hw213`):
  - `test_ui_layout_scope`: 6 cases, 17961 assertions. Zero tolerance for every
    answer about every element, compared outside a scope and inside one, three ways:
    structure only, memo filled in tree order, memo filled in reverse order. Run on
    the authored canvas and on a scaled one, plus auto-size and scroll extents over
    3 frames, and the text cache.
  - Negative controls, each rebuilt and confirmed by the DLL timestamp:
    - Box memo +0.25 and child index +1 fail 1969 checks.
    - Auto-size without forgetting fails frames 0 and 2.
    - A text key without the wrap column fails the text test.
  - Green: `test_ui_widgets` (427/427), `test_ui_theme`, `test_ui`, `test_widget_*`,
    `test_app_*`, `test_dev_stats_tile`, `test_software_raster`,
    `test_hc_pull_on_construct` and `test_third_person_template`.
  - Full `he_tests` run: 5422 cases, 4 failed. These are the same 4 as in step 1:
    2 `inspector_ui` clipboard cases (baseline) and 2 order-dependent `EngineProfiler`
    cases.
  - `HorizonEditor` builds.
- **Not verified:** `extract` output before vs. after is compared by quad count (2847)
  only, not quad by quad; equivalence is proven per layout function. The text cache's
  share at 0.77 ms is not measured. The editor and game were not run by hand. The designer path is
  unchanged (it opens no scope). Linux, macOS and Metal: CI only.
- **Decisions taken:**
  - A scope instead of a persistent index on the tree, because the vector is public
    and nobody can enforce invalidation.
  - The table entry is checked against the element, and a miss falls back to the
    walk: a stale table is slow, never wrong.
  - A parent cycle already overflowed the stack in the old rect and visibility
    recursion. The scope does not change that and makes no claim about it.
- **Decisions suggested, not taken:**
  - CORE-702 "reuse last frame's `PointerHit`". It needs a tree revision that script
    writes, animations and scrolling all bump (CORE-705).
  - Scopes for the designer and the MCP tools.
  - The CheckBox/RadioButton/ComboBox measure cache (CORE-602 names them; only
    `UIText` is done).
- **Traps:**
  - `tests/HorizonCore.dll` is NOT refreshed by the build. Copy it after every Core
    edit; a negative control ran green against the old DLL once.
  - The Bash `sed -i` here turns a CRLF working copy into LF (git normalizes).
  - In the auto-size test, every box gets `sizeToContent`, except the "Line" box, which
    has to keep its width or the sensitivity case measures nothing.
- **Next steps:**
  1. Step 3: CORE-302/401/402/303 (script context cache), with a micro-benchmark.
  2. Optionally, scope the designer's per-frame layout (`UIEditorPanel`), measured
     first.
