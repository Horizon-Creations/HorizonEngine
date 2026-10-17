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

## Step 3: interpreter dispatch (CORE-302, CORE-303, CORE-401, CORE-402)

**CORE-402 (dispatch index).** `Inst::dispatch` lists the events and functions by
name, built on first use from `levels`, which are written only once at registration.
Every declaration is kept leaf first, in node order. That way the element filter, the
public-only skip of an inherited call, and `callOwn`'s "first entry in that level" pick
exactly what the scans picked. `levelHandlingEvent`, `levelWithFunction`,
`functionSignatureOf` and `callOwn`'s Run On lookup all read the index. The scan used
to build the event names of every Input Action node on each fire, and that was all
12 allocations of a frame without a handler.

**CORE-401 (Context per instance).** `Inst::contexts` holds one
`shared_ptr<const Context>` per level, built by `makeContext` on first use
(`Runtime::contextFor`). `Runner` gets a second constructor over the shared pointer,
and `m_ctx` is now a `const Context&`, so the compiler refuses any write to a Context
that nested Runners share. The share keeps the Context alive through a run that
destroys its own instance. A moved `Runtime` rebuilds its Contexts, because the
lambdas hold its `this`. The compiled path (`bindContext`) is unchanged.

**CORE-302 (`coerce`, audit stage 1 of 3).** There are two overloads now: `const Value&`
copies once, and only when the value passes through unchanged; `Value&&` moves through
once instead of twice. It is not timed separately: the effect is inside the noise, and
none of the saved copies allocate (SSO). The `Value` layout (stage 2) is a project of
its own.

**CORE-303 (class cache).** `resolveClassAsset` keeps each clean walk: every class's
`graphJson` and `baseClass` as read, plus the new `TypeRegistry::generation()`. A hit
re-reads the chain and compares strings, without parsing. Validating instead of
invalidating means there are no hooks in `ContentManager`. Saves no longer go through
hot reload (step 1), a base can appear after its derived class, and the compare catches
both. A walk that hit a missing base or a cycle is not cached, so it keeps reporting.
Callers still get their own copy.

**Measurement** (`test_hc_dispatch`, `--no-skip`, Release, MacBook M5, libc++). 1000
instances of a two-level class with about 110 nodes per level. Three runs each:

| case | before (3e8e2dcf) | after (cd1eecaa) |
|---|---|---|
| Tick inherited, 33 nodes | 29.0-30.6 ms, 878-926 ns/node, 21 allocs/inst | 22.3-25.3 ms, 675-768 ns/node, 6 allocs/inst |
| Tick, no handler | 5.4-5.9 ms, 12 allocs/inst | 0.13 ms, 0 allocs |
| host call of a base function | 8.6-9.0 us, 4 allocs | 4.5-5.4 us, 1 alloc |
| spawn through `resolveClassAsset` | 3.12-3.16 ms, 9844 allocs | 53-61 us, 30 allocs |

The load average was 4.5-14 (other bees), so the times are ranges. The allocation
counts are exact: `malloc_logger` on the calling thread, which also sees the
`HorizonCore` dylib. 401/402 alone (eb91b990): no-handler 0.13 ms, tick 26.4-28.3 ms,
call 5.2-6.7 us, spawn unchanged. What remains of a ticking frame (~700 ns/node) is
node evaluation: `evalInput` scans all links, and `pinRanges`/signatures are rebuilt
per input (CORE-301, not this step).

## Handoff (step 3)

- **Topic / step:** 213, step 3 of 4
- **Branch / commits:** 3e8e2dcf (bench, before), eb91b990 (401/402), 42f656de (302),
  cd1eecaa (303), plus this doc.
- **Files touched:**
  - `HorizonCodeRuntime.h`: `DispatchIndex`, `Inst::contexts/contextsOf/dispatch`,
    `contextFor`, `dispatchOf`.
  - `HorizonCodeRuntime.cpp`: `dispatchOf`/`levelHandlingEvent`/`levelWithFunction`
    at the end, `contextFor` after `makeContext`, `functionSignatureOf`, `callOwn`,
    the five Runner sites.
  - `HorizonCode.h`/`.cpp`: the `Runner` constructors and `m_ctxHold`/`m_ctx`;
    `coerce` (anonymous namespace before `pullTypeWord`).
  - `HcClassResolve.cpp`: `CachedClass`, `chainUnchanged`, `resolveClassAsset`.
  - `TypeRegistry.h`/`.cpp`: `generation()`.
  - `tests/test_hc_dispatch.cpp` (new): 10 cases plus the bench.
- **Verified** (macOS, Release, `out/build/release`):
  - `test_hc_dispatch`: 10/10, 103 assertions. The 6 level-choice cases were green
    against the old scan before any change.
  - Negative controls, each rebuilt:
    - Without the elem filter and the public-only filter, 2 cases fail.
    - A root-first index fails 5 of 6.
    - A cache without the compare fails the edit case.
    - Caching unclean walks fails the late-base case.
  - The HC/engine-API/host/widget/ContentManager suites are green: 1085 cases.
  - The full `ctest -j4` run: 302/303, 2 skipped. The one failure is `test_hpak`
    "Disk registry", which also fails on its own (6 == 2). `scanContentDirectory` also
    scans the user's real `EngineContentCache` (4 `.hasset` files on this Mac); that is
    environment, not this step: with `HOME=<empty temp dir>` the case passes 9/9
    (lesson 382; CI's cache is empty).
- **Not verified:** HorizonEditor and HorizonGame were not built here (Scene was, through
  he_tests). No Runner is constructed outside Core/tests. Windows/Linux: CI only. The
  editor and game were not run by hand. A spawn in a real project with structs: the
  registry is written only on struct load and editor actions, so the cache should hold
  during play; this is not measured.
- **Decisions:** validate by content instead of invalidation hooks. The cache is global
  by class path; another ContentManager simply fails the compare. Leaf private shadows
  base public for host calls (`callFunction` with `publicOnly=false`): this is existing
  behaviour and is pinned by a test, not changed.
- **Traps:** an edit test has to write the content system's own copy
  (`getHorizonCodeClassMutable` + `saveAsset`), the way `saveClassState` does. A fresh
  asset saved over the path never reaches the loaded copy, cache or not. On the Mac the
  dylib is picked up directly; the `HorizonCore.dll` copy trap applies to Windows only.
- **Next steps:**
  1. Step 4: CORE-403, the hash.
  2. Later: CORE-301 (evalInput link index, cached pin ranges), the biggest remaining
     share of a tick. CORE-408 (`Inst::levels` as shared const graphs) would also save
     the remaining copy on spawn. Delete the unused non-const `Inst::leaf()` with it:
     `DispatchIndex` holds `const Node*` into `levels`, so "never written after
     registration" should become something the compiler checks.
  3. The class cache keeps each ancestor's `graphJson` once per derived class that
     was resolved (a base with N subclasses: N copies). That was deliberate; if it
     shows up in a large project, key the links by asset instead.

## Step 4: the pak hash (CORE-403)

**Hash.** `Hpak::hash64` is now FNV-1a over 64-bit little-endian words in four
lanes, so four multiplies are in flight instead of one per byte. After each multiply,
`h ^= h >> 32` feeds the high half back down. Plain word-wise FNV only carries a
difference upward: two flips of bit 63 in the same lane cancel. The lanes fold in
order, then the tail (whole words, then bytes), the length, and a murmur3 finalizer.
The old function is still there as `fnv1a64Bytes`.

**Format (audit option B).** `Hpak::k_version` 3, `k_oldestVersion` 2; the layout is
unchanged. This is a bump by the `HAsset.h` rule: an older reader would refuse every v3
package as *corrupt*. `HpakReader::Toc` records the version. `open` checks the TOC with
`hash64ForVersion`, `readStoredBytes` the content the same way, and `openShared`
compares against the TOC's version, not the build's. `readStoredEntry` hands out the
*current* hash, so a v2 entry that incremental packing carries verbatim into a v3
package stays valid (`addPackedEntry` copies it as is). `kPackPipelineVersion` 3
(`ProjectExporter.cpp:316`) throws away the old export manifests. Their
`srcHash`/`inHash` were byte-wise.

**Not switched:** ContentSync (`EngineContentPublish.cpp:94,239`) uses `fnv1a64Bytes`
on purpose. Its manifest lives on the server, and a new function would make every
published entry look changed. The comment at `EngineContentManifest.h:36` says so.
Options A (no hash for encrypted entries) and C (hash per 4 MiB block) were not done.

**Measurement** (Release, MacBook M5, **low-power mode on**, otherwise idle):

| | before | after |
|---|---|---|
| hash over 256 MiB (`Hash bench`, `--no-skip`) | `fnv1a64Bytes` 0.25-0.26 GB/s | `hash64` 4.22-4.25 GB/s (~17x) |
| streaming 128 x 1 MiB zstd (`Streaming bench`) | wall 280-293 ms | wall 221-259 ms |
| streaming 4000 small entries | wall 88-93 ms | wall 88-90 ms (no hash share worth naming) |

The 0.25 GB/s is below the audit's 664 MB/s (x86, g++). That fits low-power mode,
but it was not measured with low-power mode off. A first after-run was thrown away:
another bee had all 10 cores busy with `yes` (topic 208).

## Handoff (step 4)

- **Topic / step:** 213, step 4 of 4
- **Branch / commits:** d2ec8b14 (code + tests), plus this doc.
- **Files touched:** `HpakFormat.h` (`k_version`, `fnv1a64Bytes`, `hash64`,
  `hash64ForVersion`), `HpakReader.h/.cpp` (`Toc::version`, `open`, `openShared`,
  `readStoredBytes`, `readStoredEntry`), `ProjectExporter.cpp:316`,
  `EngineContentPublish.cpp`, `EngineContentManifest.h`, `tests/test_hpak.cpp`
  (end of the file, 6 cases plus a skipped bench), `tests/fixtures/hpak_v2_sample.h` (new).
- **Fixture:** 13097 bytes, written by the *unchanged* HpakWriter at d83c87f2 before any
  Hpak edit, from `v2FixtureBlob`/`v2FixtureSettings` (Store, LZ4, zstd,
  Store+AES). No current writer can make it again; do not regenerate it.
- **Verified** (macOS, Release, `out/build/release`):
  - `test_hpak` 59/59, 767 assertions (empty `HOME`, lesson 382).
  - `test_project_exporter` 34, `test_export_profiles` 50, `test_integrity` 11,
    `test_engine_content_manifest_refresh` 4: all green.
  - Negative controls, each rebuilt; each fails exactly where expected:
    - A: TOC check always uses `hash64`. The 4 version cases fail.
    - B: carry-over keeps the old hash. The carry-over case fails.
    - C: `openShared` compares against `k_version`. The v2 read case fails.
    - D, standalone: without the xor-shift, the two top-bit flips at offsets 7/39
      collide.
  - Full `ctest -j4`: 302/303. The only failure is the known `test_hpak` "Disk
    registry" case (6 == 2, from the 4 `.hasset` files in this Mac's EngineContent
    cache, lesson 382); with an empty `HOME`, `test_hpak` passes 59/59.
- **Not verified:** an editor export of a real project over a v2 pak (the exporter
  tests cover the manifest path, not the old-pak reuse path). The ContentSync publish
  path against a server (it only compiled). HorizonEditor/HorizonGame not built
  locally. Windows/Linux: CI only.
- **Decisions:** option B with a version bump, no new dependency (no XXH3). ContentSync
  stays byte-wise. A v2 package stays readable indefinitely (`k_oldestVersion`); a
  re-export writes v3.
- **Traps:** `hash64` is now on-disk format. The test pins two values; changing the
  function means `k_version` 4 plus a `hash64ForVersion` branch.
- **Next steps:** stage 1 of the topic is done with this step. Options A and C only if a
  profiler still shows the hash on the read path.
