# LandscapeFoliageLayer: new asset type for per-layer foliage spawn, design (Thema 327, step 1)

Design only. No code, shader or asset was changed. Every `file:line` below was re-read in this worktree on 2026-10-29 (branch
`claude/neuer-asset-typ-landscapefoliagelayer-…`, on top of Thema 233 @ `9c1b7f5b`). Claims are tagged **[code]** (read from the
code), **[doc]** (taken from a design doc) or **[inferred]** (reasoned, not run). Section 9 lists what is left unverified.

## 0. Decision in one paragraph

**LandscapeFoliageLayer is an authored rule set, referenced by UUID, that is *applied* to a landscape's `FoliageComponent`.** It is
`LayerMeshPresets.h` as data: a list of entries, each one a `FoliageLayer` minus its per-scene state. "Apply" copies the entries into
persisted `FoliageLayer`s (tagged with where they came from), so scatter, buckets, clusters, LODs and pooling stay exactly the existing
`FoliageSystem` / `FoliageExtract.cpp` path. **No new instancing, LOD or pooling code, and no new rule fields**: the slope, height and
paint rules this step was told "do not exist yet" already exist on this branch (§2). The Material-Graph node is a **zero-output metadata
node** that names the asset for a landscape material (like `LandscapeLayerBlend` names the paint layers); it spawns nothing and shades
nothing. The file is a normal `.hasset` in `EditorDeps/EngineContent/`, so the existing publish and passive-pull mechanism carries it
with zero changes to `HE_ContentSync` (§7).

## 1. What the topic's words map to

| Topic's word | In the engine | Where |
|---|---|---|
| "Landscape-Layer" | a **texture group** = the terrain's material id (Mountains / Forest / Grassland, Desert in flight) | `AutoLandscapeMaterial.h:158` [code], 233 doc §1.3 [doc] |
| "Schicht innerhalb eines Layers" | a **role / paint channel k** of that material (0 Auto, 1..4 the four roles, 5 Puddles, 6..7 free); `FoliageLayer::weightLayer` | `FoliageComponent.h:105` [code]; same channel = same role in every group, 233 §3 [doc] |
| "Foliage-Typ" | one `FoliageLayer` (mesh + LODs + material) | `FoliageComponent.h:36` [code] |
| "Gewichtung/Dichte" | `density` (instances per m²) and the painted `densityMask`; see §3 on why there is no separate `weight` | |
| "Regeln Steigung/Höhe" | `slopeMin/Max`, `heightMin/Max` | `FoliageComponent.h:92-103` [code] |

## 2. What already exists (the Thema 180 overlap, settled)

* The foliage plan reserved slope/height/paint rules "for Thema 180" (`docs/foliage-instancing-lod-plan-2026-10-10.md:310`) and said they
  hang as fields on `FoliageLayer` (`:277-278`) [doc]. Thema 180 built them: `FoliageLayer::slopeMin/Max`, `heightMin/Max`,
  `weightLayer` (`FoliageComponent.h:92-105`), enforced as pure rejection in `FoliageSystem.cpp:238-282`, pinned by
  `tests/test_foliage_rules.cpp` [code]. **This topic adds no rule fields**; it adds a data *source* for them. That answers "are
  slope/height rules part of this topic": no, they are in the branch; the asset only stores values for them.
* `LayerMeshPresets.h` already is this asset in C++: `layerMeshKindsOfGroup` (`:105`) is "which foliage types a group offers",
  `applyLayerMeshPreset` (`:145`) sets rules from the group row (`rockSlope`, `snowHeight`, `AutoLandscapeGroup`, `AutoLandscapeMaterial.h:141-152`) [code].
  Presets are compile-time and engine-only; a user cannot author or share one, and a custom landscape material has no set at all.
* **Apply precedent**: `applyGroupMeshes` (`TerrainTools.cpp:255-290`, button `:1495`) adds the group's missing kinds, never deletes, reuses
  an empty layer first, one undo snapshot "Apply Group Meshes". Identity of "already there" is the level-0 mesh id (`:262-265`) [code].
  The new asset inherits these semantics; the identity rule changes (§4).
* Layer budget: `FoliageComponent::kMaxLayers = 32` (`FoliageComponent.h:133`), the collab mask sync relies on the bound [code].

## 3. Data model (chosen reading: authored rule set, referenced by UUID)

Rejected alternatives: **scene data only** (a list on the terrain: nothing to share between scenes, nothing for a custom material
or the EngineContent library to ship); **a spawner object in the graph** (§5).

```
LandscapeFoliageLayerAsset {            // one JSON document, one chunk
  version: 1
  name, description
  entries[]  (<= 32, see budget) {
    name                              // the layer's inspector label
    meshAssetId, materialAssetId      // UUIDs; null material = FoliageLayer's own default
    lods[] { meshAssetId, materialAssetId, distance, density }   // = FoliageLod, FoliageComponent.h:21
    paintLayer   int  (-1 | 0..7)     // = weightLayer: the role channel that gates the entry
    slopeMin, slopeMax, heightMin, heightMax                     // = the layer rules, defaults "no rule"
    density, minScale, maxScale, drawDistance, bucketSize        // = FoliageLayer fields
    castsShadow, contributesAO, shadowDistance, lodHysteresis
  }
}
```

* **An entry is a `FoliageLayer` without `seed`, `densityMask`, `visible`, `dirty`, `maskRes`**, i.e. without per-scene state. The seed
  stays per scene (`addLayer` hands each layer its own, `FoliageComponent.h:181`), so two landscapes using one asset do not scatter alike.
* **`paintLayer` is an index, not a name.** Channel k keeps its *role* across groups while the names differ ("Rock" vs "Mossy Rock",
  `AutoLandscapeMaterial.h:170`), so a name match would break on a group switch [code]. The editor shows the material's name next to the index.
* **No separate `weight` field (decision, open to flip).** A relative weight between entries on one channel is just their density ratio; a
  second knob would multiply the same number. The asset-level **`densityScale`** (one slider on the whole set, default 1) covers "make
  this biome sparser". If the human wants "share of a budget", it is `density = budget × share` computed at apply time, no new runtime field.
* **Not in scope: targeting a hash *variant*** (Thema 180's per-hex variant 0..V-1, `AutoLandscapeMaterial.h:107-112`). The scatter is a CPU
  placement; the variant is a per-pixel hash. Open question 5.
* **Budget [code+inferred].** One entry = one `FoliageLayer`; its LOD levels draw as clusters per visible bucket (instanced runs of one
  mesh are one draw). An asset's entries plus the user's own layers must stay <= 32; a terrain has one group, so a set holds <= ~12 in
  practice (today's sets have 4-6, `LayerMeshPresets.h:109-118`). `k_matMaxDraws = 1024` (`VulkanRenderer.h:504`) caps *graph-material
  draws per frame* and, per the foliage lessons, only bites the per-plant graph *loop* (`HE_GRAPH_INSTANCING=0`), not clusters [doc: hive foliage lessons +
  `foliage-instancing-lod-plan` §15, `:801`]. D3D12's own constant was not read (**unverified**, §9).

## 4. Apply model (chosen: A, materialize into persisted layers)

* **A (chosen).** "Apply" writes each entry into a `FoliageLayer` on the terrain's `FoliageComponent` and tags it with
  `{sourceAssetId, sourceEntry}`. Re-apply **updates** the tagged layer's asset-owned fields (mesh, LODs, rules, density, scale, flags) and
  keeps its `seed`, `densityMask`, `visible`; it never deletes a layer and never touches an untagged one. Add-only as today, plus update.
  Cost: editing the asset does not change a landscape until Apply is pressed again (a button, one undo snapshot, as "Apply Group Meshes").
  Benefit: `SceneSerializer`, `TerrainSync` (one grid id per layer), `kMaxLayers`, the painted masks and `test_foliage_rules` are untouched
  [code: `SceneSerializer.cpp:176-259` writes a field only when it differs from `FoliageLayer{}`], and the packaged game needs only the
  layers, not the asset.
* **The tag is the one data change**: two optional fields on `FoliageLayer`, written only when non-null, so every old scene saves
  byte-identical (the rule the serializer already follows, `:176-178`). It replaces the level-0-mesh identity of `applyGroupMeshes`,
  which cannot tell two entries with the same mesh and different rules apart (Grassland scatters `Pebble` and `RockA` with its own
  numbers). Whether `TerrainSync` needs to carry the tag is **unverified** (§9).
* **B (rejected): derive layers at load from the asset, not persisted.** Edits would propagate, but painted masks and seeds then have no
  home, `TerrainSync`'s per-layer grid ids and the 32-cap need rework, and it is a second way to make layers next to the first. Not for 0.8.0.
* Applying needs the asset *resident*: the button loads it with `loadAssetAsync` (the passive-pull route, §7) and is disabled while the
  download is pending; it must not call the synchronous `ensureResident`, which does not trigger a pull [code: `ContentManager.h:488-492`].
* **Engine sets as data, with a parity test (step 2).** The default engine assets are generated from the existing presets
  (`layerMeshKindsOfGroup` + `applyLayerMeshPreset`), and a test pins "layer from the asset == layer from the preset" per kind, so the data
  form cannot drift from the C++ form before the C++ one is retired (not in this topic).

## 5. Material-Graph node (chosen: zero-output metadata node)

Options:

| | Option | Verdict |
|---|---|---|
| 1 | **Spawner node**: a node that places foliage while the graph runs | No. Foliage placement is CPU scatter on the terrain, not shading; five backends would need a compute or CPU hook. |
| 2 | **Weight/mask input node** next to `LandscapeLayerBlend` (a pin that feeds a density) | No. The rules judge slope/height/paint on the CPU; the shader's weights are not readable there. Two sources of truth that drift. |
| 3 | **Metadata node `LandscapeFoliage`**: no pins, `s` = asset reference; the material *declares* its foliage set like it declares paint layers | **Chosen.** Matches "the material is the single source of truth for what a layer is" (`TerrainTools.cpp:48-50`, `MaterialGraph.h:571-577`) [code]. |
| 4 | No node, only a field on the terrain | Rejected: the topic asks for a node, and a custom landscape material could not carry its set. |

Mechanics (step 3):

* **Enum**: append `LandscapeFoliage` to `MatNodeType` (`MaterialGraph.h:36`). Node types serialize by *name*, so the append is free (`:34-35`) [code].
  Name tables / palette: `MaterialGraph.cpp` name map, `MaterialEditorPanel.cpp:576` (the `LandscapeLayerBlend` switch) are the places to mirror.
* **Extraction needs a pre-pass, not the emit path.** `layerNames` is filled while *emitting* the blend node (`MaterialGraph.cpp:1437-1450`, then
  `gen.layerNames = …` at `:2655`), and emission is demand-driven from the Output pins (`emitNode` recursion, `:1832`, `:2041`). A node with
  no output is never reached, so it would be dropped silently **[inferred from the call structure, not run]**. Hook: a scan over `g.nodes` for the
  node type in the generator, next to `:2655`, into a new `MaterialGenerated::foliageSet` (beside `layerNames`, `MaterialGraph.h:577`). Step 3 must
  prove it with a graph where the node is unwired and still reaches `MaterialAsset`.
* **Reference form: a UUID, not a path.** The asset's own refs are UUIDs too. Reason **[code]**: `HpakWriter.cpp:364-368` rewrites path references to
  baked UUIDs only for Mesh, Material, Scene and ParticleSystem blobs, and inside a Material only for the chunks it knows (MREF/MTLU…); a path string
  in a new MTRL tail field or in a new asset type's JSON would not be rewritten and would dangle in a pak.
* **`MaterialAsset` field**: `foliageSetId` (UUID), appended **after `customShaderDispGlsl`** in the MTRL tail (`ContentManager.cpp:2110`,
  read at `:354`). The packer re-folds the GI-approx block and copies *only what sits behind it* verbatim (`ContentManager.cpp:2108-2109` comment,
  `HpakWriter.cpp:572-574`); anywhere earlier the field would be lost or shift the approx block [code]. Old files end earlier, the reader leaves
  the default (`readPOD` is bounds-checked). No `k_version` bump (§6). Instances (`ContentManager.cpp:907`, `:933`) copy `graphLayerNames` from
  the parent; `foliageSetId` must be copied at the same two sites.
* **Engine groups are *not* re-generated with the node.** `buildAutoLandscapeGraph` builds Mountains/Forest/Grassland; adding a node changes
  their graph JSON and committed `.hasset` bytes, which breaks 233's invariant "Mountains is today's material, byte for byte" and the perf
  baselines taken against it (lesson: landscape regression) [doc]. Instead each `AutoLandscapeGroup` row gets a `foliageSetId` (`AutoLandscapeMaterial.h:141-152`),
  and "Apply Foliage" resolves **node first (custom materials, user override), then the group row**. The node is therefore what a *user*
  sets on their own landscape material; the engine's three stay untouched.
* **Editor UX** ("möglichst einfach per Node"): the node's inspector is a drop slot for the asset (via `EditorWidgets`, with an
  `EditorHelp.cpp` entry, `editor_help_audit.py --check`); the Landscape tool's Setup tab shows "Foliage set: <name>" and the Apply button
  next to "Apply Group Meshes" (`TerrainTools.cpp:1495`).

## 6. Serialization: new asset type, one JSON chunk, no version bump

* **`AssetType`**: append `LandscapeFoliageLayer` after `Sequence` (`Enums.h:113`). Numbers are append-only (Sequence = 26, so the new one is 27);
  the number is written into the `.hasset` header (`HAsset.h` `FileHeader::asset_type`, uint16) and into the pak's `__asset_types__` [code].
* **Three switches in `Enums.h`, no default labels by design**: `isCollabSyncableAssetType` (`:131`) → **true** (authored, small, two people tune
  it live, like BlendSpace/Theme), `assetTypeName` (`:194`) → `"LandscapeFoliageLayer"`. A missing case is exactly the warning the comments predict.
* **Chunk**: new 4CC `LFOL`, UTF-8 JSON, the BoneMask/BlendSpace/Theme pattern: read `ContentManager.cpp:487-506`, write `:2205-2225`,
  register `:2793-2795` (`registerBlendSpace` is the template), getter pair at `:2688`. An unparseable chunk is a **failed** load, not an empty set
  (the Sequence rule, `:650-655`), so a save cannot overwrite the author's file with nothing [code].
* **Version**: `HAsset.h:25-37` states a version bump is for a change that breaks "old readers stop before it / old files end before it"; a
  whole new chunk of a new asset type is neither, so **`k_version` stays 2** [code]. An old build meets an unknown header type and does not load it
  (it never had it); nothing existing changes.
* **`__asset_types__` (`ProjectExporter.h:374-379`)**: the pak writer takes every packed asset's type from the file header (`HpakWriter.h:131-139`,
  `packedTypes()`); `HpakWriter.cpp:364-368` returns any type outside Mesh/Material/Scene/ParticleSystem untouched (`return blob; // no refs to
  rewrite`) [code]. So the index needs **no change** and the asset packs verbatim. Under apply model A the game does not even need the asset at runtime.
* **Other registration places** (from the files that mention `BlendSpace`, `grep -l` over `src/`, [code]): `Assets.h` (struct), `ContentManager.h` (storage,
  getters), `HE_Core/CMakeLists.txt` (new `*.cpp` for JSON parse/validate), `AssetStubWriter.cpp` (create from the Content Browser),
  `ContentBrowserPanel.cpp` (create menu, icon, colour), `InspectorPanel.cpp` (asset inspector), `McpToolsAsset.cpp` (MCP type lists), `EditorHelp.cpp`,
  `tests/`. Step 2 lists these; steps 3-4 add the node and the apply button.

## 7. EngineContent and HE_ContentSync (existing mechanism, no new upload path)

* **Location**: `EditorDeps/EngineContent/LandscapeFoliage/LF_<Group>.hasset` (today's tree: `Audio MaterialFunctions Materials Meshes Particles Widgets`,
  checked with `ls`). Engine path `Engine/LandscapeFoliage/LF_Forest.hasset`; remote/manifest path `LandscapeFoliage/LF_Forest.hasset`.
* **Publish [code]**: `EngineContentPublish.cpp:73-78` scans `EditorDeps/EngineContent` with `recursive_directory_iterator` and keeps a regular file only if
  `extension() == ".hasset"`. There is **no directory filter and no type filter**; the UUID is read from the file (`readAssetUuid`, `:27`, used `:86`), the hash
  is FNV-1a over the bytes, the manifest entry is `{path, uuid, contentHash, size}`. The server-side rebuild (`:209-240`) also keys on the extension only
  (non-`.hasset` become raw entries with no UUID). A new type in a new folder is therefore published with **zero sync changes**. `HE_ContentSync` has no
  `AssetType` reference at all (grep of `EngineContentSync.cpp` / `EngineContentPublish.cpp`).
* **Pull [code]**: the editor registers every manifest entry with a UUID as a remote asset (`EditorApplication.cpp:3496-3530`, path prefixed `Engine/`,
  `registerRemoteAsset` `ContentManager.h:472-495`); the first `loadAssetAsync` of that UUID enqueues the one download queue (`enqueueDownload`, `Passive`).
  So the auto-pull fires when something *requests the UUID*. Under model A that is the **Apply button** (and the node's drop slot/preview), which is exactly
  when the file is needed. Nothing needs adding to `SceneSystems::collectAssetRefs` (`SceneSystems.cpp:256-300`): scenes reference the *layers'* meshes
  and materials, which that function already lists including LODs (`:270-274`). Should B ever be chosen, the terrain/material reference would have to be added there.
* **Materials that carry `foliageSetId`**: `ContentManager::expandFrontier` follows baked UUID refs of a *loaded* asset (`:1930-1950` region; Sequence is the model). Step 3
  decides whether a material's `foliageSetId` is a streamed dependency (then the file pulls with the material) or only fetched on Apply. **Recommended: only on Apply** (the set is
  not needed to *draw* the material; making it a dependency pulls a file into every scene that uses the material).
* **Explicit route**: the Content Browser's confirmed double-click download is the path-driven route and works for any manifest path, including this folder.
* **Meshes the default sets use** are already engine files (`EditorDeps/EngineContent/Meshes`, ids `0x100..0x12A`, `DefaultAssets.h:120-145`) and pull the same way.

## 8. UUID block (this is live, not hypothetical)

* The lesson on this spot says `0x41D..0x41F` are the only free ids in the 0x400 block. That is exactly three, and `landscape-3-0-text-6` is adding a **Desert** group
  right now (hive board: `Desert group: table row (0x4A0..0x4A3)`). Three defaults would exhaust the block and leave Desert without a set.
* **Recommendation: a fresh block `0x4B0..0x4BF`** (after Desert's `0x4A0..0x4A3`; 16 ids: one per group now, room for more), with a comment in
  `AutoLandscapeMaterial.h` at the group table and a pin in `tests/test_landscape_groups.cpp` (which already pins uniqueness). **Hazard [lesson]:** the ContentManager keeps one entry per UUID
  whatever its type, so a clash silently loads the new asset as the wrong type (`{0x413,1}` once loaded the Forest material as a MaterialFunction). Do not use `0x413/0x414`
  (weather functions). **Needs the human's OK** (open question 3); posted to the topic so text-6 sees it.
* Ids of the default assets are `{0x4B0 + groupIndex, 1}`; `hi < 0x10`-style sentinels are not needed because `generate()` UUIDs never look like these (`DefaultAssets.h:6-9`).

## 9. Read from code vs inferred, and what was not verified

**Read from code:** the enum, the three `Enums.h` switches, the ContentManager read/write/register patterns, `HAsset.h` version rule, `FoliageLayer` fields and the
rules in `FoliageSystem.cpp:238-282`, `LayerMeshPresets.h`, `applyGroupMeshes`, `AutoLandscapeMaterial.h` group table and id comment, the publish scan rule (extension `.hasset`
only), the rebuild rule, the passive-pull registration, the HpakWriter pass-through for unlisted types and the MTRL-tail copy rule, `layerNames` extraction on the emit path.

**Inferred, not run:** (a) an unwired metadata node is never visited by emit (call structure only); (b) pak UUID of a project asset equals its file UUID, so a UUID reference in
an MTRL tail survives packing (the engine sets use fixed ids, so this matters only for user assets); (c) a draw-count claim for clusters vs `k_matMaxDraws` rests on the
foliage lessons, not on a measurement here.

**Not verified:** D3D12's draw-cap constant and the other renderers' (only Vulkan's was read, per the no-D3D/Vulkan-on-Mac rule they cannot be run here); whether `TerrainSync`
must carry the new layer tag; whether `SceneSerializer`'s `foliageLayerToJson` gets the optional fields without touching `tests/test_foliage_rules.cpp` expectations; no build or test was
run (doc only).

## 10. Open questions for the human

1. **Node semantics.** Chosen: metadata node naming the set, *only for custom/user landscape materials*; the three engine materials keep their graph and use the group-row id. OK, or should
   the engine materials be regenerated with the node (changes their bytes and the perf baseline)?
2. **Apply: explicit or automatic?** Chosen: explicit button + update-on-reapply. Should assigning a group/material to a terrain with no foliage layers apply its set automatically?
3. **UUID block.** `0x4B0..0x4BF` for the default sets (and Desert already on `0x4A0..0x4A3`), or reuse `0x41D..0x41F`?
4. **Slope/height rules in this topic?** Chosen: no new rule fields, they exist (§2); the asset stores values for them. Confirm no further rule kinds (aspect, distance to water, noise) are wanted now.
5. **Variants.** Targeting one of the hash variants of a role (Thema 180) is out of scope. Wanted later?
6. **No `weight` field** (density ratio + asset-level `densityScale` instead). OK?

## Handoff

- **Topic / step:** 327 LandscapeFoliageLayer, step 1 of 5 (design).
- **Branch / commit:** `claude/neuer-asset-typ-landscapefoliagelayer-foliage-spawn-pro-land` @ see `git log -1` (pushed: see result post).
- **Done:** design record for the asset's data model, `.hasset` form, graph node, relation to `FoliageSystem`, EngineContent path. Doc only.
- **Files touched:** `docs/landscape-foliage-layer-entwurf-2026-10-29.md` (new; nothing under `src/`, `tests/`, `scripts/`).
- **Verified:** every cited path/line re-read in this worktree; publish scan rule read in `EngineContentPublish.cpp:73-78`, `:209`; packer pass-through read in `HpakWriter.cpp:364-368`.
- **Not verified:** see §9 (unwired-node extraction, D3D12 cap, TerrainSync tag, pak UUID of user assets). No build run.
- **Decisions taken:** authored rule set by UUID (§3); apply model A, materialize + tag (§4); metadata node, UUID ref, MTRL tail after `customShaderDispGlsl` (§5); new type `LFOL` chunk, no `k_version` bump, collab true (§6); no sync change (§7); no new rule fields, no `weight` field.
- **Decisions suggested, not taken:** UUID block `0x4B0..0x4BF`; node only for custom materials; auto-apply on group assign. All in §10 for the human.
- **Pending:** answers to §10; step 2 implements the type; step 3 the node; step 4 the apply logic.
- **Traps:** the task text said the rules "do not exist yet", they do (`FoliageComponent.h:92-105`). `0x41D..0x41F` looks free but Desert (text-6) is also taking ids. Emit never visits an unwired node. Appending anywhere but after `customShaderDispGlsl` in the MTRL tail breaks packaged materials. Path refs inside a new asset's JSON are not baked by the packer; use UUIDs.
- **Next steps:** 1. Step 2: `AssetType` + 2 switches (`Enums.h:113,131,194`), `Assets.h` struct, `LFOL` chunk read/write/register in `ContentManager.cpp` (copy BlendSpace), JSON parse/validate with a test that loads a bad chunk as a failed load. 2. Generate the default sets from `layerMeshKindsOfGroup`/`applyLayerMeshPreset` into `EditorDeps/EngineContent/LandscapeFoliage/`, parity test per kind. 3. Verify a local publish scan lists the file (read `EngineContentPublish.cpp:73`, simulate the scan; no server). 4. Step 3 node, with the unwired-node test first.
