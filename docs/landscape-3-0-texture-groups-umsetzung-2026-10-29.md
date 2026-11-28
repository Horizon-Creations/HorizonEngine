# Landscape 3.0: texture groups implemented (Thema 233, step 2)

Implements decision B of `docs/landscape-3-0-texture-groups-entwurf-2026-10-29.md`: a texture group is a landscape material
with its own three arrays; the group of a terrain is the id of its `MaterialComponent::materialAssetId`. Nothing is serialized
new, nothing group-specific reaches a backend.

## What exists now

* `AutoLandscapeMaterial.h`: `AutoLandscapeGroupId {Mountains, Forest, Grassland}`, `AutoLandscapeGroup` row
  (`materialId/Path`, `arrayPaths[3]`, `paintLayerNames`, `roleLabels[4]`, defaults `rockSlope/dirtAmount/snowHeight`),
  `kAutoLandscapeGroups[]`, `autoLandscapeGroup(id)`, `autoLandscapeGroupOfMaterial(uuid)` (-1 = not a group),
  `autoLandscapeGroupOfArray(path, &map)`. Mountains' row *is* the old constants (`kAutoLandscapeMaterialId` 0x412, the three
  `T_Landscape_*_Array`, `kAutoLandscapePaintLayerNames`), which stay.
* `buildAutoLandscapeGraph(view, group = Mountains)`; the skeleton is untouched, the group changes the three array strings, the layer-blend
  name string and three parameter defaults. `kAutoLandscapeVariantGroups` was renamed `kAutoLandscapeRoleCount` (the four roles).
* Generated assets (committed): `EditorDeps/EngineContent/Materials/M_AutoLandscape_{Forest,Grassland}.hasset`, ids
  `{0x415,1}` / `{0x419,1}`; arrays (NOT committed, `.gitignore`, server-only like Mountains') `T_Landscape_<G>_{Albedo,Normal,Mask}_Array`
  on `0x416..0x418` / `0x41A..0x41C`. Placeholders: 128², V=1 (5 slices), in the group's own tones.
* `landscape_tex_gen <dir> --material --group Forest` and `<dir> --group Forest [--variants N]` (arrays only; `--pack/--loose` are Mountains only).
* GI: `giAutoLandscapeParams` takes the albedo array of any group. `ContentManager::noteLandscapeArrayDepth` checks each group's three arrays
  against each other. Witness: `HE_DUMP_AUTOLANDGROUP=Forest|Grassland` with `HE_DUMP_AUTOLAND=1` (and `..ARRIVE`).
* Tests: new `tests/test_landscape_groups.cpp`; Forest/Grassland in the cross-compile sweep (`test_material_graph.cpp`, "Auto landscape material (Forest)");
  group case in `test_landscape_reload.cpp`.

## Handoff

* **Branch / commit:** `claude/landscape-3-0-textur-layer-gruppen-forest-grassland-mountain`, work in `351dc7e2` (+ this doc).
* **Verified (built and run, Release, Metal Mac):**
  * Mountains: `landscape_tex_gen --material` output is `cmp`-identical to the committed `M_AutoLandscape.hasset`; the existing tripwire tests
    ("shipped M_AutoLandscape is exactly what the builder makes", "baked GLSL ...") pass.
  * `ctest -R 'test_auto_landscape_variants|test_material_graph|test_landscape_reload|test_landscape_groups|test_engine_dependencies|test_culling|test_project_exporter'` 7/7.
    `test_landscape_groups` pins: table/ids unique (also against 0x400..0x414), same node types and links as Mountains, own arrays only, 13 params,
    the shipped Forest/Grassland files equal the builder's graph, their baked GLSL is the codegen's today, every array path is found by
    `EngineDeps::findEngineReferences` (= the editor prefetch and the pak's refs), bakes on all RendererBackends (cross-compile only for D3D11/D3D12/Vulkan).
  * Full `ctest -j4` in the Release tree: 342/343. The one red, `test_hpak` ("scanContentDirectory == 2", got 7), counts the five assets of this Mac's real
    `EngineContentCache`; it passes with `HOME=<empty dir>`. Not caused by this step (an environment trap for any machine with a populated cache).
  * Headless witness on **Metal and OpenGL**: Forest and Grassland draw (`draws=4`, no "Cannot load asset") in their own tones with placeholder arrays;
    arrays absent = the known white-array hexagons (control, `Cannot load asset` x5). Shots were in `/tmp/l3shots`, not committed.
  * `EditorApplication.cpp` compiles (HorizonEditor linked in the build tree).
* **Not verified:** D3D11 / D3D12 / Vulkan at runtime (no hardware here; only the cross-compile sweep + bake above). No pixel A/B of Mountains
  before/after (the graph text and the asset bytes are identical, so none was run). Pak export of a project that uses Forest was not run end to end
  (the exporter packs the whole EngineContent dir, the test proves the refs are in the material file). The nested `HorizonSDKRelease` build of the editor
  target needs a network download (steam-audio) and failed once; I configured with `-DHE_SDK_RELEASE_SET=OFF`.
* **Decisions taken:** ids Forest `0x415` (+arrays `0x416-0x418`), Grassland `0x419` (+`0x41A-0x41C`); `0x413/0x414` are the weather functions (design
  §5.6 said `0x413..` was free: wrong, see Traps). Defaults: Forest rock slope 0.10, dirt 0.50, snow height 2000; Grassland 0.20 / 0.20 / 2000
  (suggested values, untuned, the placeholder looks say nothing about them). Group arrays are placeholder-generated in memory, no per-layer source
  textures written. No separate "prefetch" list exists: `EngineDependencies` scans material bytes for `Engine/...hasset`, so the group arrays are
  fetched/exported through the materials; the old `EditorApplication` block at `~9108` is only the late-arrival witness (it now takes the group).
* **Pending (human, unanswered):** shipping V and texture size per new group (832 MiB at 2K V=3 RGBA8, design §5.2); the source textures for Forest/Grassland (§5.3);
  mixed groups in one terrain (option C, §5.1). The Forest/Grassland arrays still have to be uploaded to the EngineContent server (SFTP, outward-facing; not done).
* **Traps:** (1) `0x413/0x414` clash: the ContentManager keeps one entry per UUID, a landscape material on `{0x413,1}` loaded as a MaterialFunction, `getMaterial`
  null, the Forest graph compiled with 12 params. Free now: `0x41D..0x41F`. (2) `test_landscape_reload` must use stand-in array names: the real
  `T_Landscape_*_Array` may sit in the machine's EngineContent cache and resolve before anything "arrives". (3) Do not `touch` anything to rebuild; adding the
  test file only needed a CMake re-run. (4) The terrain tool still lists the material chip generically (`TerrainTools.cpp:~1322`): no picker yet (step 3).
* **Regenerate the group files:** the generator is deterministic. `landscape_tex_gen <dir> --material --group Forest` (the mode flag must be argv[2], before
  `--group`; `<dir> --group Forest --material` silently writes arrays instead) and `landscape_tex_gen <dir> --group Forest` for the arrays; same for Grassland.
  Until the arrays are on the EngineContent server a terrain bound to a group draws the white-array hexagons (the "absent" witness).
* **Deferred on purpose (step 3's first item):** a test that binds a terrain to Forest's id and loads a scene saved before groups as Mountains with identical weights.
  No serializer or weight code was touched and Mountains' id `0x412` is unchanged; only the hook `autoLandscapeGroupOfMaterial` is pinned here.
* **Next steps:** step 3: group picker in `TerrainTools.cpp` (list `kAutoLandscapeGroups`), paint list from the material's `graphLayerNames` (already per group),
  switch-group + old-scene-loads-as-Mountains test (`autoLandscapeGroupOfMaterial(kAutoLandscapeMaterialId) == 0` is the hook; a scene stores the material id only).
  Step 4: mesh kinds per group read the group row's defaults (`rockSlope`, `snowHeight`). Step 5: hardware acceptance, V/size decision, upload arrays.
