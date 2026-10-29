# Landscape 3.0: several texture groups on one terrain (Thema 233, step 7), WIP

Status: **stopped by the queen part-way.** The CPU data model and the paint API are written and syntax-checked;
nothing is built, no test exists, no UI, no serializer, no renderer. Read the Handoff.

## Binding route (the first thing the step asked to settle)

* Six separate arrays (two groups x 3 maps) cannot be bound. `kMatMaxGraphTextures = 4` (`MaterialGraph.h:581`), the
  Vulkan descriptor code pins it (`VulkanRenderer.cpp:9492` `static_assert(... == 4, "heTexP0..3 occupy bindings 4..7")`),
  `MaterialGraph.cpp:938` caps the slots, and the D3D sampler table is full (plan `auto-landscape-material-2-plan.md` §3.1;
  `heTexP3` is the only free register, one array, not three). Three of the four slots are a group's own arrays today.
* The route that fits without a new binding scheme is design §1.4 option A: one **fat array set** in the same three slots,
  `slice = (groupIdx * 4 + role) * V + v`, and a dedicated mix material (ids from 0x4A4; arrays follow the material id + 1 + map).
  Cost: memory (G=4, V=3, 2K RGBA8 is about 3 GiB resident, V=1 about 1 GiB; use V=1 and small arrays in tests), a new graph (the
  Groups pin 4 -> 16, depth no longer says V, so the mix material needs V baked in), and a per-hex pick against the cell-centre
  share or a blended evaluation (cost not measured). **Not built.** Mountains' graph, ids and pins were not touched.
* Until that exists, a terrain with a stored share still renders as its own group: the LandscapeLayerBlend normalises over the
  material's *declared* layers only (`MaterialGraph.cpp` `wsum`), so bytes in layers 6/7 are harmless to a six-layer material.

## Data model (written)

* `TerrainComponent::mixGroup[2]` (`TerrainComponent.h`, after `layerWeights2`): `AutoLandscapeGroupId` of the group whose share
  sits in weight layer 6 / 7; -1 = none (default = legacy, one group). The own group (`materialAssetId`) is the remainder
  `1 - s6 - s7`, so an unpainted texel is the own group alone.
* `TerrainPaint` (`TerrainPaint.h/.cpp`): `reservedLayers`, `isReservedLayer`, `groupShares`, `setMixGroup`, `paintGroup`,
  `fillGroup`, `paintInGroup`. `paint()` normalises layers 0..5 only when shares are reserved and refuses a reserved layer;
  `fillLayer()` keeps the share bytes. With no mix group both are byte-for-byte the old code paths (index arrays `ch[]`
  replace the loop bounds; **not yet proven by a test**).
* Manual painting, intended semantics: a stroke on a role layer in a mixed terrain also moves the *active group's* share
  (`paintInGroup`), so "Forest Mossy Rock here" = role Rock + group Forest. A group-only stroke is `paintGroup`.

## Handoff

**Done** (uncommitted before this doc; commit "WIP" on the branch):
* `TerrainComponent.h` field, `TerrainPaint.h/.cpp` API above. `c++ -std=c++20 -fsyntax-only` on `TerrainPaint.cpp` is clean
  (`-Isrc/HE_Scene/include -Isrc/HE_Core/include -Isrc/HE_Rendering/glm`). Nothing else compiled.

**Not done, in this order**
1. `SceneSerializer.cpp` write (~732-742) / read (~1752): key `mixGroups` as `[a, b]`, written only when one is >= 0; read default
   -1, clamp to `0..kAutoLandscapeGroupCount-1`, drop a second slot equal to the first. Old scenes must load as one group.
2. `TerrainSystem.cpp:93-120` `computeAverageLayerWeights`: with reserved layers, normalise layers 0..5 without the share bytes and
   store the (scaled) shares in `avgLayerWeights[6/7]` (`sharesOf` logic); keep the old code when `reservedLayers == 0`.
3. `TerrainTools.cpp`: Setup "Mix Groups" row under the group picker (`~1474`; two 5-cell `segmented` rows: None + the four
   groups, the own group and the other slot's group disabled; call `TerrainPaint::setMixGroup` after `snapshotNow`); Paint tab
   "Active Group" selector (own + mixes) above the layer list (`~1578`), stroke at `~798` becomes `paintInGroup(..., slot, ...)`,
   Ctrl+drag = layer 0 + slot 0; "Fill With Layer" at `~1631` and the two "Clear Paint" (`~1555`, `~1639`) must decide about the
   shares (Clear Paint should clear them; say so in its help). Help entries in `EditorHelp.cpp` for every new label, then
   `python3 scripts/editor_help_audit.py --check`.
4. Checks the step listed: `McpToolsTerrain.cpp:816-882` (builds its own terrain JSON, must carry `mixGroups`),
   `TerrainSync.cpp` (raw bytes, `replaceKeepingGrids` at :292 takes the blob's `mixGroup` from the new side: read, believed fine),
   `FoliageSystem.cpp:178` / `weightLayer` 6/7 now read a share (a foliage layer gated by 6/7 grows where that group is; the own
   group has no layer, decide whether to add a pseudo layer).
5. `tests/test_terrain_mix_groups.cpp` (+ `tests/CMakeLists.txt`): paint without mix equals the old bytes (copy a fixture from
   `test_terrain_paint.cpp`), reserved layers refused, `fillLayer` keeps shares, `paintGroup` sums, `setMixGroup` fold/clear,
   scene round-trip, scene without the key loads as one group. Then `he_tests` (TerrainComponent.h is included by 63 files: one
   rebuild, batch the header edits) and the ctest list from the assignment.
6. Fat-array mix material (route above) + five-backend witnesses: separate step, needs the human's call on shipping sizes.
   Vulkan / D3D11 / D3D12 stay unverified (not possible on this Mac); do not mark them done from CI.

**Decisions**: shares live in weight layers 6/7 as the design says (reuses the 2:1 weightmap upload and TerrainSync with no new
grid or sampler); the group set is per-terrain `mixGroup[2]` and not tied to Forest/Grassland, because Desert is a fourth group and
two channels hold two secondary groups. **Only suggested**: the combined role+group stroke, the foliage reading.

**Traps**: do not use `mv` to swap files, do not `touch` headers; `TerrainComponent.h` rebuilds ~63 files. No `Grep` tool in this
session, use `/usr/bin/grep`. zsh: `echo ====` fails, quote it.
