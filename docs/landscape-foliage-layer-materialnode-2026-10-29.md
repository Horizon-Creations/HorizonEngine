# LandscapeFoliageLayer: Material-Graph node (Thema 327, step 3)

Implements §5 of `landscape-foliage-layer-entwurf-2026-10-29.md` as designed (metadata node, UUID ref, MTRL tail after
`customShaderDispGlsl`). Branch `claude/neuer-asset-typ-landscapefoliagelayer-foliage-spawn-pro-land`, commits `7cebc43f`, `688c7f3b`.

## What exists now

* **Node** `MatNodeType::LandscapeFoliage` ("Landscape Foliage", category Landscape), appended after `FbmGrad`
  (`MaterialGraph.h`, registry entry at the end of `kReg` in `MaterialGraph.cpp`). No pins, no params. `s` = the asset UUID as 32 hex
  digits (`matFoliageSetIdToString/FromString`; malformed or empty = none).
* **Generator**: `generateFragment` scans the top-level graph for the node (lowest id with a valid set wins) into
  `MatShaderGen::foliageSetId`, before the no-Output early return. It sits outside the demand-driven emit on purpose: an output-less node is
  never reached from the Output. The GLSL (forward and G-buffer) is byte-identical with or without the node, so setting it never recompiles a pipeline.
* **Asset field** `MaterialAsset::foliageSetId`: written as two `uint64` as the LAST MTRL tail field (after `customShaderDispGlsl`),
  read bounds-checked (old files = none), set in `regenerateMaterialFromGraph`, copied by both instance paths in `syncMaterialInstance`.
  `HpakWriter` needed no change (it copies everything behind the re-folded approx block verbatim); a pack round-trip test proves it.
* **Editor**: the node body is an `EditorWidgets::assetDropSlot` for `LandscapeFoliageLayer` (picker + Content-Browser drop + Clear),
  `MaterialEditorPanel.cpp` `drawNodeBody`; it asks for the set once with `loadAssetAsync` so the slot can show a name for a
  not-yet-resident set. Help key `Material Node/Foliage Set` in `EditorHelp.cpp`. MCP: `LandscapeFoliage` in the node-name switch,
  `foliageSet` in `nodeJson`, a `requires` line in the node catalogue.
* **Tests**: `tests/test_landscape_foliage_node.cpp` (9 cases: registry, id text round-trip, unwired node reaches the generator and the
  shader is unchanged, lowest-id rule, graph JSON, save/load/regenerate, old-file tail, instance copy, packer ship) and one case in
  `test_material_editor_ui.cpp` (real panel, headless frame shows "Forest Foliage" + Clear; `HE_UI_DUMP_DIR` writes the BMP).
  `test_material_graph.cpp`'s every-node-has-a-registry-entry loop bound moved to `LandscapeFoliage`.

## Handoff

- **Topic / step:** 327, step 3 of 5.
- **Done:** see above. Commits `7cebc43f` (node, field, editor, MCP, help, tests), `688c7f3b` (UI test).
- **Verified (macOS arm64, Release, Ninja, shaderc ON, `out/build/release`):** he_tests builds clean; `test_landscape_foliage_node.cpp` 9/9 (79 assertions);
  `test_material_graph.cpp` 106/106; `test_material_editor_ui.cpp` 4/4 and the frame viewed (the zero-pin node lays out, slot shows the set name + Clear);
  full suite with `HOME=/tmp/leer`: 5971 cases passed, 0 failed, 19 skipped; `scripts/editor_help_audit.py --check` green;
  `MaterialEditorPanel.cpp` / `McpToolsMaterial.cpp` `-fsyntax-only` clean (both are also compiled into he_tests).
- **Resolved from the design's unverified list:** (b) a user asset's pak UUID equals its file UUID: `HpakWriter.cpp:224` `metaFromHasset` takes the
  entry id from the file's own META, so a UUID in the MTRL tail survives packing. (a) unwired node never visited by emit: not needed, the scan is explicit, and the
  test proves the node reaches `MaterialAsset` while unwired.
- **Not verified:** HorizonEditor target not built here (only the two changed editor files syntax-checked, plus the UI test linking the panel); no live
  Editor interaction (drag-drop from the real Content Browser, undo of a node slot change); D3D/Vulkan not run (no shader change, so nothing backend-specific);
  CI matrix not yet seen (workflow run started after the push, see the result post).
- **Decisions taken:** `s` as hex UUID text (not a path: the packer bakes only known refs); the slot uses `undo=false` because the graph editor's own
  `structuralEdit` snapshot covers it; `foliageSetId` is NOT a streamed dependency of the material (design §7 recommendation), the slot requests the set once for display only.
  Skipped on purpose: `EditorApplication.cpp:7218` (engine group build must stay byte-identical for Thema 233) and the preview scratch-material copies.
- **Not done (out of scope, step 4):** `AutoLandscapeGroup::foliageSetId` group rows (and with them the UUID block `0x4B0..0x4BF`, still unanswered by the human),
  the Apply button / "Foliage set: <name>" readout in the Landscape tool's Setup tab, any spawning. The test uses `0x4B0` only as an arbitrary id.
- **Traps:** a fresh `LandscapeLayerBlend` has TWO layers (`Layer 1\nLayer 2`), not one. A node with no pins and no params gets no body in the panel
  unless `nodeValueHeight` says so (it does now, 30). If a later step wants the node to also drive a group's set at generation time, keep it out of
  `buildAutoLandscapeGraph`: that changes the engine materials' bytes.
- **Next steps:** 1. Step 4: read `MaterialAsset::foliageSetId` of the terrain's material (node first, then a group-row fallback once the human settles the id block), load the
  set with `loadAssetAsync`, apply into `FoliageComponent` with the tag from design §4. 2. Add the readout + Apply next to "Apply Group Meshes" (`TerrainTools.cpp:1495`).
  3. Build `HorizonEditor` once before step 5.
