# Landscape 3.0, step 4: layer meshes per texture group (2026-10-29)

Topic 233, step 4 of 5. Design: `landscape-3-0-texture-groups-entwurf-2026-10-29.md` §4.

## What it does

* `LayerMeshPresets.h`: `LayerMeshKind` gained `Fern, ForestStump, FallenLog, MossRock, TallGrass, FlowerTuft, ScreeChip` at the end
  (the six old values keep their numbers). `layerMeshKindsOfGroup(group)` is the per-group list; `layerMeshGroupOfMaterial(id)` finds the
  group from a terrain's material (Mountains when it names none); `layerMeshKindOfStem` is the witness's name lookup.
* `applyLayerMeshPreset(layer, kind, group = Mountains)`: rock start, flat-ground limit and snow line come from the group's row
  (`AutoLandscapeGroup::rockSlope`, `snowHeight`). Rock start = `rockSlope + 0.03` rounded to the hundredth, which gives exactly 0.15 for Mountains.
* Sets: Mountains `Tuft, Pebble, RockA, RockB, RockC, SnowDrift` (unchanged); Forest `Fern, ForestStump, FallenLog, MossRock`;
  Grassland `TallGrass, FlowerTuft, Pebble, RockA, ScreeChip`.
* Ids (`DefaultAssets.h`, mesh_gen table index + 0x100): Fern `0x119` (3 levels), ForestStump `0x11C` (2), FallenLog `0x11E` (2), MossRock `0x120` (3),
  TallGrass `0x123` (3), FlowerTuft `0x126` (3), ScreeChip `0x129` (2). Files: 18 new `.hasset` in `EditorDeps/EngineContent/Meshes` (not uploaded).
* Editor: Inspector `Mesh Preset` lists the entity's group's kinds (`InspectorPanel.cpp` ~3622); Landscape Setup has `Apply Group Meshes`
  (`TerrainTools.cpp`, `applyGroupMeshes`) that only adds missing kinds (a layer whose mesh is null is filled first); `HE_DUMP_AUTOLANDMESHES`
  accepts any kind's stem and uses `HE_DUMP_AUTOLANDGROUP`'s numbers.

## Handoff

* **Topic / step:** 233, step 4 of 5. Branch `claude/landscape-3-0-textur-layer-gruppen-forest-grassland-mountain`, commits `d33e373e` (presets, ids, mesh_gen, 18 files, tests), `69b9a45f` (editor), docs commit after.
* **Verified:** pin test (`tests/test_layer_mesh_groups.cpp`) written first and green against the OLD header, green again after the refactor; two negative
  controls (Pebble `slopeMax` 0.25 -> 0.26, table `rockSlope` 0.12 -> 0.13, in scratch overlays) turn it red. Full `he_tests` 5951 cases / 0 failed (fresh
  empty HOME). `HorizonEditor` builds. `editor_help_audit.py --check` 1244/1244. The 17 existing layer meshes + CrossCard regenerate byte-identical; mesh_gen is
  deterministic across two runs. Ids checked by grep over `src tests` for `0x119..0x12F` (nothing), mesh_gen's index check extended with the 7 names (43/43),
  and the contiguity/uniqueness test.
* **Not verified:** no live editor with a renderer: the picker, `Apply Group Meshes` (only driven by the headless UI test) and the new shapes were never looked at.
  No Pixel/visual check of Fern/Stump/Log/MossRock/TallGrass/FlowerTuft/ScreeChip; use `HE_DUMP_AUTOLANDMESHES=fern,foreststump,... HE_DUMP_AUTOLANDGROUP=Forest`.
* **Decisions taken:** ScreeChip belongs to Grassland (Mountains is pinned to exactly six); Grassland borrows Pebble and RockA with its own thresholds;
  `Apply Group Meshes` is a button (not automatic on group change), it only adds; plants sit 2 cm below the ground like the stones.
* **Not done / suggested:** per-group density differences for the shared kinds ("few stones" in Grassland): not done, density is per kind. Surface materials of
  the meshes (Thema 180 §8.5). Upload of the 18 files to EngineContent (outward-facing, human).
* **Traps:** never copy `mesh_gen`'s whole output over `EditorDeps/EngineContent/Meshes`: the 8 primitives (Cube..Torus) differ from the committed files; only ids
  >= 0x108 are byte-stable. The help audit needs the entry key to be `Landscape/<button label>` with `helpForLabel` (a `helpForKey` under another name stays
  uncovered). `test_layer_mesh_instancing`'s `Land` assumes a group's kinds (default Mountains); the draw-call test would break if it took every enum value.
  Editing `DefaultAssets.h` rebuilt about 10 targets here, not the whole tree.
* **Next steps:** step 5 (hardware acceptance, all five backends, performance budget); first look at the new shapes in a live editor; human answers from §5 of the design doc.
