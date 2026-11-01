# Landscape 3.0: texture groups (Forest / Grassland / Mountains), design (Thema 233, step 1)

Design only. No code, shader or asset was changed. It refines `docs/auto-landscape-material-2-plan.md` §5.1, §6, §8 and the two
Thema 180 handoffs (`…-variants-2026-10-21.md`, `…-layer-meshes-2026-10-21.md`). Every `file:line` below was re-grepped in this
worktree on 2026-10-29; anything not read is marked **unverified**.

## 0. Decision in one paragraph

A **texture group is a landscape material**, chosen per terrain. Forest, Grassland and Mountains are three materials built by the
same `buildAutoLandscapeGraph` skeleton with a group argument, each reading **its own three texture arrays**. Inside a group the
automatic slope/height rules, the four roles, the hash variants and the paint channels are exactly the ones Thema 180 shipped. The
shader, the sampler count and the paint data model do not change. **Mountains is today's material, byte for byte.** The rejected
alternatives (one fat array set with a group index, a per-texel group map) are in §1.4 with their cost, so the queen can flip the decision.

## 1. Data model

### 1.1 What exists (verified)

* One material, one array set. Roles are called "variant groups" in the code: `kAutoLandscapeVariantGroups = 4` (Grass, Dirt, Rock, Snow),
  slice `g * V + v`, WetGround last, `V = depth / 4` (`AutoLandscapeMaterial.h:103-126`). **Naming trap:** the code's "group" is a *role*;
  this doc's "texture group" is a *terrain theme*. Step 2 should rename the constant (roles) before adding the new word.
* Variant per hex from the hash, not from slope (`AutoLandscapeMaterial.h:107-110`, `AutoLandscapeMaterial.cpp:111-144`).
* Weights are **8 channels**: `kTerrainMaxLayers = 4 * kTerrainWeightPages`, 2 pages (`TerrainComponent.h:18-19`), uploaded as one 2:1 RGBA8
  texture (`TerrainPaint.h:56-63`). The auto material names **six** of them (`AutoLandscapeMaterial.h:90-92`: Auto, Grass, Dirt, Rock, Snow,
  Puddles); 6 and 7 are free. Sum is held at 255 over both pages (`TerrainPaint.h:29`, `TerrainPaint.cpp:119-128`).
* The Landscape tool lists the layers by the **material's** names (`TerrainTools.cpp:1309`, `MaterialAsset::graphLayerNames`). The material is
  the source of truth for what a layer is; the terrain stores only weights (comment at `TerrainTools.cpp:1304-1308`).
* Parameters: 13 of `kMatMaxParams = 16` (`AutoLandscapeMaterial.h:144`, `MaterialGraph.h:614`). Textures: `kMatMaxGraphTextures = 4`
  (`MaterialGraph.h:581`), three used, `heTexP3` free.
* **D3D sampler table is full**: s0..s15, a 17th live sampler drops every graph material to built-in PBR (plan §3.1, `docs/…-plan.md:143-170`).
* "Mountains": no such name exists in code or assets today (grep: only the unrelated `MountainShape` in `TerrainTools.cpp`). Today's
  Grass/Dirt/Rock/Snow set, with rock on slopes and snow above a height, **is** the mountain look. The human's note ("bisheriges Mountains bleibt
  für komplexere Bergtexturen") is read as: the existing material becomes the Mountains group and is not rebuilt.

### 1.2 The group

A group is a row in one engine table (new, `AutoLandscapeMaterial.h`, step 2):

```
LandscapeGroup { id, name, materialId, materialPath, arrayPaths[3], roleLabels[4], defaults{rockSlope, rockBlend, dirtAmount, snowHeight, ...},
                 meshKinds[...] }          // meshKinds: §4
```

| Group | Material | Arrays | Content |
|---|---|---|---|
| **Mountains** (default) | `M_AutoLandscape` `{0x412,1}` (`AutoLandscapeMaterial.h:80-81`) | `T_Landscape_{Albedo,Normal,Mask}_Array` (`:83-85`) | existing files, unchanged |
| **Forest** | `M_AutoLandscape_Forest` | own 3 arrays | floor/leaf litter, roots/earth, mossy rock, forest snow |
| **Grassland** | `M_AutoLandscape_Grassland` | own 3 arrays | meadow grass, dry/bare soil, small rock, snow |

* **Roles stay four and dense**, in the same slice order: `0 Flat`, `1 Patch/earthy`, `2 Steep`, `3 High/Snow`. Role 3 stays *snow* in every
  group because weather snow cover (`MF_WeatherSnow`, `AutoLandscapeMaterial.cpp:279-294`) paints with that slot; a group that should be
  snow-free raises its "Snow Height" default instead.
* "Several textures per slope" = role × variant: the role is the slope/ground class chosen by the rules (§2), the variant (V = 1..3) is the
  hash pick inside it. Forest example: flat = floor / litter, patch = bare earth and roots, steep = mossy cliff.
* Slice layout **unchanged per group**: `depth = 4 * V + 1`, `slice = role * V + v`, WetGround last (`AutoLandscapeMaterial.h:99-117`). The
  "Groups" pin stays the constant 4 (`AutoLandscapeMaterial.cpp:114`). Group *g's* arrays are just another triple of files with this layout.
* Paint-layer names are group-specific because the `LandscapeLayerBlend` name string is (`AutoLandscapeMaterial.cpp:185-193`):
  `"Auto\nForest Floor\nRoots\nMossy Rock\nSnow\nPuddles"`. Channel k means the same *role* in every group, so `AutoLandscapePaintLayer`
  (`AutoLandscapeMaterial.h:91`) stays valid.

### 1.3 Where the group lives, and determinism

* **Per terrain: `MaterialComponent::materialAssetId` of the terrain entity.** The group *is* that id (reverse lookup in the table). No new
  serialized field, no collab change: a material assignment is part of the component blob, the grids still travel as `TerrainSync` patches only
  (lesson "Collab + landscapes", topic 198). Old scenes keep their id and are Mountains.
* Determinism on five backends is trivial: the choice is **which asset is bound**, made on the CPU before any draw; there is no new shader
  decision. Within a group the existing integer hash / `& 255u` rules apply as shipped.
* The tool's "Material" chip (`TerrainTools.cpp:1322`) gets a group picker that lists the table, not arbitrary materials.
* Group-specific tuning (Forest's rock slope, dirt amount, snow height) is a **different default value baked into that group's asset**, not
  a new parameter. Parameter budget stays 13/16; the slot count is not multiplied by the group count. Per-entity overrides still work on top
  (`MaterialComponent.h:22-26`, `paramOverrides`).

### 1.4 Rejected options, with cost

| Option | What | Cost (why not for 3.0) |
|---|---|---|
| **A. One material, group index as a parameter** | one array set of `G*4*V+1` slices, `slice = (group*4+role)*V+v`; Groups pin 4 → 12; `group` from a `paramOverrides` entry | the Auto-material graph text changes, so every pin on it moves (`tests/test_material_graph.cpp:2559-2562` build it; the FNV pin in `test_auto_landscape_variants.cpp:106-113` is on a synthetic array graph, not on this material). **All groups resident**: 2K RGBA8, G=3, V=3 is 37 slices × 21.33 MiB × 3 maps = **~2.3 GiB** (G=3, V=1: 832 MiB; V=2: 1.6 GiB; plan §5.2 for the 21.33 MiB), BCn is `failed` on Thema 180. G and V cannot both be recovered from the depth alone (25 = 4·6+1 is G1V6 or G3V2 …), so the material would need G baked in and old 5/13-slice arrays would mis-read. One group per terrain anyway. |
| **C. Per-texel group map** (mix groups inside one terrain) | channels 6/7 as group shares (Forest, Grassland; Mountains = remainder, so unpainted = legacy) | Needs A's fat array set *and* a pick per hex (hash against the cell-centre share) or a ×2..3 tap count. `TerrainPaint` assumes one 255-sum over all 8 channels (`TerrainPaint.cpp:119-128`; `fillLayer` clears page 2, `:143-149`; `usesSecondPage`), so 6/7 would need an exemption in paint, fill, undo, `texelWeights`, TerrainSync and the foliage `weightLayer` 6/7 meaning (`FoliageComponent.h:99-105`). A custom landscape material with 7-8 declared layers already uses 6/7 as paint, so the reservation must be declared by the material. A new texture does not fit (D3D 16/16). **Possible later step**, not part of 3.0. |
| **D. Group from a per-vertex attribute** | no sampler needed | resolution tied to chunk mesh and LOD, needs a vertex-format change on five backends. Not pursued. |

Consequence of B, stated plainly: **one terrain = one group.** Mixing groups = two terrain entities (the 100 km world plan has per-tile terrains
**unverified**: whether a tile border between two groups needs a blend is an open question, §5).

## 2. Automatic choice inside a group

* The skeleton is the Thema 180 one and is **not touched**: slope = `1 - N.y` (`AutoLandscapeMaterial.cpp:105`), dirt patches + belt below the rock
  (`:227-253`), rock past "Rock Slope" with a height-map bias (`:255-261`), snow above "Snow Height" and not on cliffs (`:263-294`), puddles as an overlay
  (`:334-`). The role (which texture class) is decided here per pixel; the variant is the hash pick (`AutoLandscapeMaterial.h:107-110`).
* A group differs by **(a) which files the roles read, (b) default parameter values, (c) role labels.** `buildAutoLandscapeGraph(view, group)`
  takes the row from the table (new argument, default Mountains, so the old call sites and `kAutoLandscapeMaterialPath` build the identical graph).
* Proposed defaults, for the author to tune after the first image (suggested, not measured): Forest raises "Dirt Amount", lowers "Rock Slope"
  a little (rocky boles show early), "Snow Height" high; Grassland lowers "Dirt Amount", raises "Rock Slope" (hills stay green), "Snow Height" high;
  Mountains = today's values.
* If a group needs a rule the skeleton lacks (e.g. a *second* slope band), it must fit the four roles or become a later graph change; adding a fifth
  role costs a paint channel (6), shrinks the free channels to one and changes the slice count `5*V+1`. **Not in 3.0** (§5).
* Weather: snow and puddles are group-independent overlays, role 3 and the Puddles channel, so weather works in every group unchanged.
* Backends: nothing group-specific reaches a backend. Each group is an ordinary graph material; the existing generation and the five backends' array
  paths are used as they are (plan §2.2 table).

## 3. Manual painting

* **Storage does not change.** A stroke paints one of channels 1..4 (the group's four textures) with the existing `TerrainPaint::paint`
  (`TerrainPaint.h:22-39`): weights stay normalised to 255, painting a texture takes weight from the others proportionally. No per-group mask: there is
  one weight set per terrain and a terrain has one group.
* **It overrides the automatic choice locally and by weight:** the surface is `Auto × automatic + Σ layer × texture`
  (`AutoLandscapeMaterial.cpp:296-332`). A painted texel has less Auto weight, so the rule fades out under the brush and the painted texture
  shows through, with the strength of the stroke and the soft brush edge.
* **Undo to unpainted = paint layer 0 "Auto"**, which is also what an unpainted texel is (default 1×1 weightmap `(1,0,0,0)`, `TerrainPaint.h:14-18`;
  header comment `AutoLandscapeMaterial.h:43-55`). This is the Thema 180 rule and it carries over unchanged. Editor undo of a stroke is the
  existing one.
* **Switching the group on a painted terrain** keeps every weight; channel k keeps its *role*, so "Rock-painted" becomes the new group's steep
  texture. The picture jumps, the data is not lost. Switching back restores the look.
* Painting a **texture of a different group** is not possible by design (the material would need both arrays). Hence the "active group" in the
  tool is the terrain's group; the layer list is that group's labels.
* GI flat-shading reads layers 0..3 only (`TerrainComponent.h:106-112` comment); pre-existing, groups do not change it. Each group's
  `approxLayerColor` comes from its own graph.
* Back-compat test needed in step 3: a scene saved before groups loads as Mountains with identical weights (`SceneSerializer.cpp:732-742` keys are untouched).

## 4. Layer meshes per group

* The Thema 180 model stays: **one mesh kind = one `FoliageLayer`**, rules `slopeMin/Max`, `heightMin/Max`, `weightLayer` (`FoliageComponent.h:92-105`).
  `weightLayer` keeps meaning "paint channel k of this terrain", and channel k is a role in every group, so **`FoliageLayer` needs no new field**
  and no serializer change.
* What is per group is the **preset set**: `LayerMeshKind` (`LayerMeshPresets.h:21`) grows kinds, and the table row lists the kinds of its group.
  The preset picker in the Inspector/Landscape tool shows the kinds of the terrain's group (group looked up from the material id, §1.3).
* Proposed sets (names and counts are a suggestion for the author):
  * **Mountains**: today's `Tuft, Pebble, RockA, RockB, RockC, SnowDrift` unchanged (`LayerMeshPresets.h:78-118`), plus `ScreeChip` (2 levels).
  * **Forest**: `Fern` (3), `ForestStump` (2), `FallenLog` (2), `MossRock` (3, rounder than RockA/B/C).
  * **Grassland**: `TallGrass` (3), `FlowerTuft` (3); reuses `Pebble` and a small rock.
* **As built (step 4, see `docs/landscape-3-0-layer-meshes-2026-10-29.md`):** Mountains stays exactly the six (a test pins every value); Forest =
  `Fern, ForestStump, FallenLog, MossRock`; Grassland = `TallGrass, FlowerTuft, Pebble, RockA, ScreeChip` (ScreeChip went to Grassland, not
  Mountains, because Mountains is pinned to six). The ids below were used as suggested (`0x119..0x12A`, all 18 files exist).
* **Ids.** Last used: SnowDrift `0x117` + 1 level = `0x118` (`DefaultAssets.h:128-133`). Next free is `0x119`; `mesh_gen` derives an id as
  `0x100 + table index` (`MeshGen/main.cpp:38`, check `:653-657`), append only. A suggested allocation, **to be fixed against the table position when
  step 4 adds the rows**: Fern `0x119-0x11B`, ForestStump `0x11C-0x11D`, FallenLog `0x11E-0x11F`, MossRock `0x120-0x122`, TallGrass `0x123-0x125`,
  FlowerTuft `0x126-0x128`, ScreeChip `0x129-0x12A`. Files go to `EditorDeps/EngineContent/Meshes` like the existing 25 (listed by `ls`).
* The preset thresholds read the **group's default parameters** from the same table row (Forest rock slope, snow height), so the rules that place a
  rock and the rule that shows rock texture cannot drift. Thema 180 hard-coded them (`LayerMeshPresets.h:106-114`).
* **Switching the group does not rewrite the terrain's foliage layers** (they are user data). The tool offers "Apply group meshes" on the
  group change (adds the group's kinds, never deletes). Decision suggested, not final: a destructive swap would lose seeds and painted masks.
* Distinct look per group is *shape and density*: Forest = ferns, stumps, mossy rounded rock; Grassland = tall grass, flowers, few stones;
  Mountains = angular rock, scree, snow drifts. Surface material of the meshes is still the later Thema 180 §8.5 item.
* Rebuild cost: edits to `DefaultAssets.h` and `FoliageComponent.h` rebuild most of the tree (Thema 180 trap). Step 4 touches only the former and
  `LayerMeshPresets.h`; do both edits in one commit.

## 5. Open questions (for the queen / the human)

1. **Mixed groups in one landscape.** B does not allow it. If wanted, option C is the route and needs the fat array set plus a paint exemption
   (§1.4). Also: a seam between two tiles of different groups in the large-world plan (**unverified** whether tiles are separate terrain entities).
2. **Memory.** Each group is its own array triple: 832 MiB at 2K, V=3, RGBA8 (plan §5.2). Two groups on screen = 1.6 GiB. BCn failed on Thema 180
   and this doc does not decide it. Options: V=1 per new group (320 MiB), 1K textures (¼), or wait for BCn. The human decides the shipping V and size.
3. **Real textures.** Forest and Grassland need source maps (plan §12, `stage_polyhaven.py`). Until then the placeholders are generated like the 128²
   ones. Which set is the human's pick?
4. **A fifth role / second slope band per group** (§2): not in 3.0 unless a group's look demands it.
5. **Group switch UX** with existing foliage layers (§4): additive "Apply group meshes" vs. ask-to-replace.
6. **Texture ids.** (**Corrected in step 2:** `0x413`/`0x414` are the weather functions, so Forest is `0x415` + arrays `0x416-0x418` and Grassland `0x419` +
   `0x41A-0x41C`; see `docs/landscape-3-0-texture-groups-umsetzung-2026-10-29.md`.) `0x413..0x41F` were thought reserved "for more landscape materials" (`LandscapeTexGen/main.cpp:93-96`): 2 materials + 6 arrays = 8 ids fit
   (e.g. Forest `0x413` + arrays `0x414-0x416`, Grassland `0x417` + `0x418-0x41A`). The per-group *source layer textures* (12+ each) need a block; which
   hi-ranges are taken beyond `0x100/0x200/0x300/0x400/0x500` is **unverified**.

## Handoff

- **Topic / step:** 233 (Landscape 3.0 texture groups), step 1 of 5 (design)
- **Branch / commit:** `claude/landscape-3-0-textur-layer-gruppen-forest-grassland-mountain` @ see `git log -1` (pushed: see the result post)
- **Done:** design doc only. Group = landscape material per terrain; Mountains = today's material; roles/variants/paint channels unchanged.
- **Files touched:** `docs/landscape-3-0-texture-groups-entwurf-2026-10-29.md` (the only file).
- **Verified:** every cited `file:line` re-grepped in this worktree (`AutoLandscapeMaterial.h/.cpp`, `TerrainPaint.h/.cpp`, `TerrainComponent.h`,
  `FoliageComponent.h`, `LayerMeshPresets.h`, `DefaultAssets.h`, `MaterialComponent.h:22-26`, `MaterialGraph.h:581,614`, `TerrainTools.cpp:1309`,
  `LandscapeTexGen/main.cpp:93-96`, `MeshGen/main.cpp:38`). `ls EditorDeps/EngineContent/Meshes` shows the 25 existing mesh files.
- **Not verified:** nothing was built or run (docs only). Whether `wsum` in the layer-blend node sums only the declared names
  (`MaterialGraph.cpp:1486` shows `page2 = names.size() > 4`, the sum was not read; only matters for option C). The FNV pin mechanism in
  `test_material_graph.cpp` (the grep found only `buildAutoLandscapeGraph` call sites). Per-tile terrains in the world plan. Free id blocks beyond §5.6.
- **Decisions taken:** B (group = material asset); roles fixed at 4; slice layout and Groups pin unchanged; Mountains = existing files; no new
  FoliageLayer field; group-specific tuning as baked defaults, not new params.
- **Decisions suggested, not taken:** option C later; additive "Apply group meshes"; mesh id allocation and kinds (§4); default parameter shifts (§2).
- **Pending:** the human's answers to §5.2 (V/size) and §5.3 (texture source).
- **Traps:** code says "group" for *role* (`kAutoLandscapeVariantGroups`); rename first. `DefaultAssets.h` / `FoliageComponent.h` edits rebuild most of the tree.
  A newer `M_AutoLandscape*` swapped into an older engine fails on missing assets (lesson, perf run notes). Do not regenerate `M_AutoLandscape.hasset`
  with a changed graph text: Mountains must stay byte-identical (check with the generator's output vs the committed file).
- **Next steps:** 1. **Step 2:** add the `LandscapeGroup` table and `buildAutoLandscapeGraph(view, group)` in `AutoLandscapeMaterial.h/.cpp`; extend
  `landscape_tex_gen` with `--group` (names, ids 0x413..); generate Forest/Grassland materials; test: Mountains graph text equal to today's,
  new groups compile on all backends (cross-translation as in the variants doc). 2. **Step 3:** group picker in `TerrainTools.cpp:~1322`, role labels
  in the paint list, switch-group test, old-scene-loads-as-Mountains test. 3. **Step 4:** mesh kinds/ids/presets per group (§4), picker by group.
  4. **Step 5:** acceptance on Metal, GL, Vulkan, D3D11, D3D12 (hardware steps stay open until NN-WS03 is there).
