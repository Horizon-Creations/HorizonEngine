# Landscape 3.0, step 6: the Desert texture group (Thema 233)

Fourth group after Mountains / Forest / Grassland, built the way steps 2 and 4 built Forest and Grassland:
a row in `kAutoLandscapeGroups`, its own material and three placeholder arrays, its own layer mesh set.
Real Desert textures are the human's job; nothing here ships art.

## What was added

| Piece | Where | Value |
|---|---|---|
| Group row | `AutoLandscapeMaterial.h` (`AutoLandscapeGroupId::Desert = 3`, count 4) | material `{0x4A0,1}`, arrays `0x4A1..0x4A3` (Albedo, Normal, Mask) |
| Paint names | same row | `Auto, Sand, Hardpan, Sandstone, Snow, Puddles` (roles 1..4 = Sand, Hardpan, Sandstone, Snow) |
| Defaults | same row | `rockSlope 0.30`, `dirtAmount 0.15`, `snowHeight 2000` (**untuned**, see below) |
| Material file | `EditorDeps/EngineContent/Materials/M_AutoLandscape_Desert.hasset` | `landscape_tex_gen <dir> --material --group Desert` (mode flag first) |
| Placeholder arrays | `landscape_tex_gen` `kGroupTones` row, in memory | not committed (like Forest/Grassland), `--group Desert` writes them |
| Mesh kinds | `LayerMeshPresets.h` (`DuneGrass, Cactus, Sandstone` appended before `Count`) | ids `0x12B..0x132` (DuneGrass 3 levels, Cactus 2, Sandstone 3), `DefaultAssets.h` |
| Desert set | `layerMeshKindsOfGroup` | `DuneGrass, Cactus, Sandstone, ScreeChip` (ScreeChip is borrowed from Grassland) |
| Mesh files | `EditorDeps/EngineContent/Meshes/{DuneGrass,Cactus,Sandstone}*.hasset` (8 files) | `mesh_gen`: `makeCactus` (ribbed tube swept along a path, two arms), `kDuneGrass` bunch, `kSandstone` stone |

## The id block (why 0x4A0)

The header said `0x41D..0x41F` were free: three ids, one short of a group (material + 3 arrays). `0x420..0x42E` are the
loose maps, `0x430..0x498` the variants (`kMaxVariants` 8 x 5 layers x 3 maps from `0x430`), `0x500` the weather sounds
and `0x413/0x414` the weather functions. `0x4A0..0x4A3` is the first free block clear of all of them. A `static_assert`
in `landscape_tex_gen/main.cpp` keeps the variants from growing into it; `test_landscape_groups` checks that the
group ids avoid the loose and variant ranges and the weather ids.

## Verified

* Build: `he_tests` and `HorizonEditor` (Release, `out/build/release`), `mesh_gen`, `landscape_tex_gen`.
* Full `ctest -j4` with an empty HOME: 344/344. Targeted: `test_landscape_groups`, `test_layer_mesh_groups`,
  `test_layer_mesh_instancing`, `test_landscape_reload`, `test_material_graph`, `test_terrain_tools_ui`, `test_terrain*`.
* `test_foliage_rules` caught the first Cactus (1.68 m, the test keeps every layer mesh under 1.2 m): the mesh was
  scaled to 1.01 m and the layer's scale range raised (0.8..2.2) instead of loosening the test.
* Byte-stability: regenerated Forest material identical to the committed file; Fern, ScreeChip, Tuft, RockA, SnowDrift identical.
* Picker: the UI test now clicks the fourth cell (material is Desert's, weights untouched); frame via `HE_UI_DUMP_DIR` shows four cells.
* `editor_help_audit.py --check` 1244/1244 (the picker uses one key for all cells, no new literal label).
* Witness `HE_DUMP_AUTOLAND=1 HE_DUMP_AUTOLANDGROUP=Desert` (`scripts/landscape-3-0-accept/cap233.sh`, Metal and OpenGL):
  no missing-asset logs, sand tones on the plateau; with `HE_DUMP_AUTOLANDMESHES=dunegrass,cactus,sandstone,screechip`
  13 draws / 87 192 tris on both backends (same counters).

## Not verified

* Vulkan / D3D11 / D3D12 runtime (no MoltenVK here, no Windows device); no backend code changed.
* The new shapes were only seen as specks from the 400 m top-down witness camera, never close up in a live editor.
* No perf measurement (Desert is one more group table row; the draw cap of 20 is covered by the instancing test).

## Decisions / untuned

* `rockSlope 0.30`, `dirtAmount 0.15`, `snowHeight 2000` are suggestions, not tuned against real Desert textures.
  Placement numbers follow the row (Sandstone/Scree start at `rockSlope + 0.03`, Cactus/DuneGrass stop at `rockSlope`).
* Cactus is a small 1 m saguaro; big ones come from the scale range. Densities (Cactus 0.0015, DuneGrass 0.05,
  Sandstone 0.005) are copies of the nearest Forest/Grassland kinds, untuned.
* Desert borrows ScreeChip from Grassland (as Grassland borrows Pebble/RockA from Mountains).

## Handoff

* **Topic / step:** 233, step 6. Branch `claude/landscape-3-0-textur-layer-gruppen-forest-grassland-mountain`.
* **Ids chosen:** material `0x4A0`, arrays `0x4A1..0x4A3`, meshes `0x12B..0x132`. Mountains/Forest/Grassland bytes and ids untouched.
* **Human steps:** real Desert textures (then pack them like Mountains; `--pack` is Mountains only), upload of the three Desert
  arrays, `M_AutoLandscape_Desert` and the 8 mesh files to EngineContent (outward-facing, not done). Until the arrays exist
  the Desert terrain draws the white fallback, like Forest/Grassland.
* **Traps:** `--material` / `--pack` must come before `--group` in `landscape_tex_gen`. Do not copy all of `mesh_gen`'s output over
  `EditorDeps/EngineContent/Meshes` (only ids >= 0x108 are byte-stable). `test_foliage_rules` limits every layer mesh to 1.2 m height.
  The editor build deploys to `out/deploy/Editor` (that is what `cap233.sh` runs).
* **Next steps:** step 7 (mixed groups per terrain); live-editor look at the Cactus/Sandstone/DuneGrass shapes; tune the Desert
  defaults once real textures exist; Vulkan/D3D acceptance on a device that has them.
