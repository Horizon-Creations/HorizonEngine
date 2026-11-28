# LandscapeFoliageLayer: Apply logic (Thema 327, step 4)

Implements §4 of `landscape-foliage-layer-entwurf-2026-10-29.md` (apply model A, materialize +
tag) and the step 3 handoff's next steps. Branch `claude/neuer-asset-typ-landscapefoliagelayer-foliage-spawn-pro-land`,
commit `2b7a7d85`.

## What exists now

* **`applyFoliageSet()`** (`src/HE_Editor/TerrainTools.cpp`, next to `applyGroupMeshes`): for each
  entry of a `LandscapeFoliageLayerAsset`, finds the terrain's `FoliageLayer` tagged
  `{sourceAssetId, sourceEntry}` or creates one (reusing an empty untagged layer first, same
  precedent as `applyGroupMeshes`), and writes every asset-owned field (mesh, material, LODs,
  rules, density × `densityScale`, scale, shadow/AO flags, `lodHysteresis`) onto it, then sets
  `layer->dirty = true`. It never reads or writes a layer nobody tagged, so a hand-added layer or
  one from a different asset/entry is untouched. `seed`, `densityMask`, `visible` are never set —
  the editor's own state survives a re-apply. It is the one place that turns the asset into
  something `FoliageSystem::update()` actually scatters; no new spawn/instancing/pooling code,
  the tagged layer is an ordinary one and goes through Thema 163/182 unchanged. `ensureResident` +
  `loadAssetAsync` is called per referenced mesh/material UUID, same as `applyGroupMeshes` does for
  the engine meshes (§7: fetched on Apply, not a streamed dependency).
* **Tag fields**: `FoliageLayer::sourceAssetId` (`HE::UUID`, `{}` default) and `sourceEntry` (`int`,
  `-1` default) in `FoliageComponent.h`. Serialized in `SceneSerializer.cpp` only when
  `sourceAssetId != {}` (same pattern as every other optional `FoliageLayer` field), so an old
  scene or a hand-added layer still saves byte-identical.
* **UI**: "Foliage Set: <name>" + "Apply Foliage" row in the Landscape tool's Setup tab
  (`TerrainTools.cpp`, right after "Apply Group Meshes"), shown only when the terrain's material
  has a non-null `MaterialAsset::foliageSetId` (the Landscape Foliage node, step 3). Not yet
  resident: asks for it once (`loadAssetAsync`, static "asked" set, same idiom as the node's own
  slot in `MaterialEditorPanel.cpp`) and disables the button meanwhile — never the synchronous
  `ensureResident` (does not start a passive pull). One undo snapshot per click, like "Apply Group
  Meshes"; unlike it, every click re-writes every entry's fields (no "nothing missing" short
  circuit) so that editing the asset and re-pressing the button is how changes reach the terrain
  (design §4's whole point).
* **Help**: `Landscape/Apply Foliage` entry in `EditorHelp.cpp`, next to `Landscape/Apply Group
  Meshes`.
* **Tests** (`tests/test_terrain_tools_ui.cpp`, two new cases, real panel headless):
  1. "Apply Foliage does not appear for a material with no foliage set" — the built-in Mountains
     material (no node) shows no button.
  2. "Apply Foliage materializes a set's entries, tags them, and re-apply refreshes them without
     touching a hand-added layer or what the author tuned on its own" — registers a
     `LandscapeFoliageLayerAsset` with two entries against **placeholder mesh UUIDs** (no real
     foliage mesh file; the task's "mit Platzhalter-Meshes testen" — real meshes come from a human
     later, design §0) and a `MaterialAsset` with `foliageSetId` set via `ContentManager::registerMaterial`
     (no disk I/O needed). Checks: the button appears and applying adds two tagged layers with the
     asset's mesh/rules/`densityScale`-multiplied density, `dirty == true`, beside an untouched
     hand-added layer; mutating the registered asset's entry and clicking again updates the tagged
     layer's name/density in place (no new layer) while a hand-set `visible = false`, `seed`, and a
     painted `densityMask` on that layer survive the re-apply.

## Decisions taken

* **Always snapshot + rewrite on click** (no "nothing to do" skip like `applyGroupMeshes`): re-apply
  is the only way an asset edit reaches the terrain under apply model A, so a silent no-op on an
  unchanged click would be surprising, and detecting "did anything actually change" is more code
  than the button is worth.
* **Node-only wiring, no `AutoLandscapeGroup::foliageSetId` row and no UUID block.** The design's
  §10 question 3 (UUID block `0x4B0..0x4BF` vs `0x41D..0x41F`) is still unanswered by the human
  across every run of this topic; this step only reads `MaterialAsset::foliageSetId`, which a user
  sets via the Landscape Foliage node on their own material (step 3's own scope line). The three
  engine group materials carry no node and so show no "Apply Foliage" row — they keep using "Apply
  Group Meshes" exactly as before. Wiring a group-row fallback is therefore deferred, not blocked:
  whenever the UUID block is settled, it is one more `if` before the node check in the same block.
* **`weightLayer` out-of-range → -1**, not rejected: an asset authored against more terrain
  channels than this landscape has is the same leniency `SceneSerializer`'s `foliageLayerFromJson`
  already gives a hand-edited scene file.
* **Placeholder meshes are never registered as resident assets** in the test (just arbitrary
  UUIDs): `applyFoliageSet`'s `ensureResident`/`loadAssetAsync` calls on an unknown UUID are a
  verified no-op (`ContentManager::loadUuidAsync`, `ContentManager.cpp:1676`, "unknown UUID"
  branch) — proven by the test passing, not just inferred.

## Verified (NN-WS03, MSVC 18.0/14.51, Ninja, build tree `C:/hw327`)

* `he_tests` full suite: 3674/3678 passed. The 4 failures (`test_hc_codegen_export_build.cpp`,
  `test_hpak.cpp`, `test_inspector_ui.cpp`, `test_physics_threading.cpp`) are pre-existing/order-
  dependent noise, not a regression: re-run with `--source-file` limited to just those four, three
  pass in isolation and only the physics crash reproduces; none touch Foliage/Terrain/MaterialGraph/
  SceneSerializer (hive lesson 591, extends `vulkan-d3d-baseline-noise.md`).
* `test_terrain_tools_ui.cpp` alone: 10/10 (both new cases included).
* `HorizonEditor` target builds and deploys clean (`C:/hw327/deploy/Editor`).
* `scripts/editor_help_audit.py --check`: 1245/1245 entries covered, `landscape` area 34/34.

## Not verified

* No live editor session (drag-drop of a real `.hasset` foliage set from the Content Browser onto
  a Landscape Foliage node, then clicking Apply in the real Setup tab) — only the headless
  `ImGuiSoftwareRaster` UI test drove the button.
* No visual check with a non-placeholder mesh (the task said to use placeholders; real meshes are
  out of scope here per design §0 — "later vom Menschen").
* D3D11/D3D12/Vulkan/Metal/OpenGL not run — this step touches no shader and no renderer code, only
  CPU-side editor/data-model code, so nothing backend-specific changed.
* CI matrix not started from here (push happens in this handoff's follow-up); no PR opened yet —
  this topic's branch still merges into `claude/landscape-3-0-textur-layer-gruppen-forest-grassland-mountain`,
  not main.

## Next steps (step 5: backend acceptance)

1. Build/test on the other platforms (Linux at least; Metal needs macOS) if the acceptance step
   wants cross-platform confirmation — this step's change is backend-agnostic C++/editor code, so a
   green `he_tests` + `HorizonEditor` build on each is the expected outcome, not a new behaviour to
   hunt for.
2. A live-editor smoke test of the full chain end to end: author a `LandscapeFoliageLayerAsset` with
   a *real* mesh (once available), put a Landscape Foliage node on a custom landscape material,
   assign it to a terrain, click Apply Foliage, confirm the plants actually render/instance through
   the existing foliage extraction/rendering path (no code expected to be needed here — this step
   proved the data reaches `FoliageComponent` the same way a hand-added layer would).
3. If/when the human answers design §10 question 3 (UUID block), step for `AutoLandscapeGroup::foliageSetId`
   rows + default `LandscapeFoliage/LF_<Group>.hasset` EngineContent files can follow; until then the
   three engine materials keep using "Apply Group Meshes" only, by design.
