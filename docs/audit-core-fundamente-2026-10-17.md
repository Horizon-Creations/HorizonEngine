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

## Handoff

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
    the 3 new cases (10 + 1 + 2 checks).
  - `test_player_host`, `test_project_exporter` and `test_asset_ref_retarget` are green.
  - `HorizonEditor` builds.
  - Full `he_tests` run: see the step's result post.
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
