# Landscape 3.0, step 3: painting a group's textures by hand (2026-10-29)

Design: `landscape-3-0-texture-groups-entwurf-2026-10-29.md` §3 (no new storage). Step 2 doc: `landscape-3-0-texture-groups-umsetzung-2026-10-29.md`.

## What exists now

* The Paint list was already per group: it draws `MaterialAsset::graphLayerNames`, and each group's material carries its own names
  (`test_landscape_groups` pins that). So a Forest-bound terrain lists Auto / Forest Floor / Roots / Mossy Rock / Snow / Puddles with no paint-list change.
  Row k writes weightmap channel k through `TerrainPaint::paint`; Auto (0) is the default and the way back.
* What was missing was a way to BIND a terrain to a group. `TerrainTools.cpp`, Setup tab: a "Texture Group" segmented control, one cell per row of
  `HE::kAutoLandscapeGroups` (nothing else; no "mixed"). `assignTextureGroup` loads the group's engine material by path, writes its id into the terrain's
  `MaterialComponent`, marks it dirty, one undo step (no-op on the group already bound). The active cell comes from `autoLandscapeGroupOfMaterial`
  (a custom material highlights none). The weightmap is not touched: one set per terrain, channel k = role k in every group.
* Paint tab: one dim line under "Layer" says which group is bound. `s_paintLayer` is clamped to the material's layer count every frame the panel draws.
* Help: `Landscape/Texture Group` in `EditorHelp.cpp` (`editor_help_audit.py --check` green, 1243/1243).

## Handoff

- **Topic / step:** 233, step 3 of 5
- **Branch / commit:** `claude/landscape-3-0-textur-layer-gruppen-forest-grassland-mountain`, see `git log` (pushed: yes, see result post)
- **Done:** a terrain can be bound to Mountains / Forest / Grassland from the Setup tab; the Paint list shows that group's names and a stroke writes only the chosen channel.
- **Files touched:** `src/HE_Editor/TerrainTools.cpp` (`assignTextureGroup` near `segmented`, picker in the Setup branch, hint line in the Paint branch),
  `src/HE_Editor/EditorHelp.cpp` (`Landscape/Texture Group`), `tests/test_terrain_tools_ui.cpp` (last test), `tests/test_terrain.cpp` (last test).
- **Verified (built he_tests Release, run):** `test_terrain_tools_ui` 7/7 (new: picker lists exactly the 3 table rows, no "mixed" cell; picking the bound
  group = no undo step; Forest = material id switched, `layerWeights` byte-identical, one undo step; Paint list = Forest's names; a stroke on row 3 sets
  channel 3 to 255 at the centre, sum 255, a far texel unchanged, earlier paint kept; Auto row paints channel 0 back), `test_terrain` 41/41 (new:
  scene with Mountains' id round-trips with identical weights incl. layer-4 page and reads as group Mountains; same scene bound to Forest has a
  byte-identical terrain JSON), `test_landscape_groups`, `test_landscape_reload`, ctest `-R scene|terrain|landscape|help|material_graph|undo` 25/25.
  Visual witness: `HE_UI_DUMP_DIR` frames `landscape-group-picker` (Mountains active, 3 cells) and `landscape-paint-forest` (Forest's six labels), looked at.
- **Not verified:** the picker in the real editor with a live renderer (`InvalidateMaterial` path; `ctx.renderer` is null in the UI test). Forest/Grassland arrays
  are local placeholders, not on EngineContent: in a real editor the terrain draws the white-array hexagons after a switch until they are uploaded. No GPU run,
  no D3D/Vulkan (step 5). The Mountains graph text / id 0x412 were not touched (no generator run needed).
- **Decisions taken:** picker in Setup next to the Material slot (not the card chip, which only displays); no per-group mask; no mixed option (human decision).
  Stale-index safety stays the per-frame clamp in the Paint branch.
- **Pending:** step 4 (per-group layer meshes), step 5 (acceptance); human answers from the step 2 handoff (shipping V/size, source textures, mixed groups, upload).
- **Traps:** a row's ImGui id in the test is `ImHashStr("##layer", 0, ImHashData(&i, 4, childWindow->ID))` (PushID(i) in the body child). `EditorUndo` has no
  `undoLabel()`, only `undoLabelAt(i)` with odd semantics: the test checks `undoDepth()` only.
- **Next steps:** 1. step 4: `FoliageLayer` mesh presets read the bound group (`autoLandscapeGroupOfMaterial(terrainMaterialId)`). 2. Decide with the human on uploading the
  Forest/Grassland arrays before anyone judges a switch visually in the editor.
