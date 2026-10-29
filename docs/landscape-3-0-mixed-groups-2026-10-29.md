# Landscape 3.0: several texture groups on one terrain (Thema 233, step 7)

Status: **done for a CPU/UI-only mix (no fat-array render material yet).** Started on the Mac (data model +
paint API, syntax-checked only, commit f9714437), finished on NN-WS03 (serializer, avgLayerWeights, editor UI,
tests, full Windows build + ctest). Read the Handoff.

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

**Done:**
* Mac (f9714437, syntax-checked only): `TerrainComponent::mixGroup[2]`, `TerrainPaint.h/.cpp` API (`reservedLayers`,
  `isReservedLayer`, `groupShares`, `setMixGroup`, `paintGroup`, `fillGroup`, `paintInGroup`; `paint()`/`fillLayer()` refuse/keep
  reserved layers 6/7).
* NN-WS03 (this run): `SceneSerializer.cpp` write (after `layerWeights2B64`, ~line 745) / read (after `layerWeights2`, ~line 1760):
  key `mixGroups` as `[a, b]`, written only when one slot is >= 0; read defaults both to -1, clamps to
  `0..kAutoLandscapeGroupCount-1`, drops a second slot equal to the first. `TerrainSystem.cpp` `computeAverageLayerWeights`:
  layers 0..5 normalise without the share bytes when reserved, `avgLayerWeights[6/7]` get the mean (clamped-to-1) share instead;
  `reservedLayers(tc) == 0` reproduces the old loop exactly (every new bit test is then always false). Verified by reading, no
  code change needed: `McpToolsTerrain.cpp`'s `terrain_paint` MCP tool (refuses a reserved layer the same "nothing happened" way
  `texelWeights` already handles, via the byte-diff check); `TerrainSync.cpp`'s `GridsAside`/`serializeWithoutGrids` (`mixGroup`
  is not a grid, rides the full-component settings blob like `resolution`/`weightRes` do; `restoreTerrain` never touches it).
  `FoliageSystem.cpp`'s `weightLayer` 6/7 now reads a group share where one is reserved — left as emergent behaviour (a layer
  gated on it grows where that group's share is) rather than special-cased; not wired to anything else this step.
* Editor UI (`TerrainTools.cpp`): Setup "Mix Groups" — two independent segmented rows (None + the four groups, the terrain's own
  group and the other slot's pick disabled) calling `TerrainPaint::setMixGroup`. Paint tab "Active Group" selector (own + both
  mix slots, shown only once `reservedLayers(tc) != 0`); the brush stroke calls `TerrainPaint::paintInGroup` with the active slot
  (Ctrl+drag always targets slot 0, the own group). Both "Clear Paint" buttons now also reset `mixGroup` to `{-1,-1}` (the shares
  lived in the bytes they clear). Two new `EditorHelp.cpp` entries (`Landscape/Mix Group`, `Landscape/Active Group`) plus an
  addition to `Landscape/Clear Paint`'s text.
* `tests/test_terrain_mix_groups.cpp` (9 cases; added to `tests/CMakeLists.txt`): default/no-mix state, `paint()`/`fillLayer()`
  byte-identical without a mix group, `setMixGroup` reserve/refuse/clear, paint normalising 0..5 only, `fillLayer` keeping shares,
  `groupShares` remainder maths, scene round-trip, pre-mix-field scene loads as one group, `paintInGroup` combined stroke.
* **Built and tested on NN-WS03** (first real Windows build of this topic — not a syntax check): fresh configure outside the
  worktree's long path (`cmake -S <worktree> -B C:\hb\l7 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=/EHsc
  -DDEPLOY_DIR=C:/hb/l7deploy`, VS "18" BuildTools + bundled Ninja, vcvars64). `cmake --build C:\hb\l7 --target he_tests -j16`:
  **EXIT=0**. This links `RendererOpenGL;RendererSoftware;RendererVulkan;RendererD3D11;RendererD3D12` into `he_tests` — a real
  *compile*, not a syntax check, of GL/Vulkan/D3D11/D3D12/Software on this change, using the real Vulkan SDK and `dxc` (D3D12
  DXR). `ctest -R "test_terrain_mix_groups|test_terrain$|test_landscape_groups|test_material_graph|test_foliage_rules|
  test_terrain_tools_ui"`: **6/6 passed**. Full suite `ctest -j4` (scratch `HOME`): **345/348 passed**; the 3 failures
  (`test_planner_ui`, `test_hpak`, `sdk_example_project`) touch none of this step's files and match the known pre-existing
  Windows-runner noise (hive lesson "Pre-existing Vulkan/D3D/ctest noise") — not investigated further here.
  `python3 scripts/editor_help_audit.py --check` (via `py -3`): **1244/1244, unchanged** (the new controls are `segmented()`
  cells / `InvisibleButton`, which the audit's regex does not scan — same as the existing Texture Group picker).

**Not done:**
* The fat-array mix material (render route, below) and any pixel/visual verification of a mix — nothing samples `mixGroup` yet,
  so Metal/GL witness scripts would show byte-identical pixels to before this step; not run.
* Runtime smoke-test of the editor itself on D3D11/D3D12/Vulkan/GL (only `he_tests`, not `HorizonEditor`, was built this session;
  no backend-specific code was touched, so this is lower-risk, but it is **not verified**).
* An MCP endpoint for `setMixGroup`/Active Group (none exists for the single Texture Group picker either — no parity gap
  introduced, but also no MCP-driven test of the new paint path).
* CI was not triggered for this specific commit from this session.

**Decisions:** shares live in weight layers 6/7 as the design's option C (reuses the 2:1 weightmap upload and TerrainSync, no new
grid or sampler); `mixGroup` is per-terrain and generic over groups (not tied to "Forest/Grassland") because Desert is a fourth
group and only two channels exist; a stroke with a mix slot active also pulls the group share toward that slot (`paintInGroup`),
including slot 0 ("Own"), so selecting "Own" and painting also reclaims a texel from the other groups' shares; "Clear Paint"
drops the mix groups along with the weightmap. Render route if a mix is ever made visible: option A's fat array set in the same
three graph-texture slots (`kMatMaxGraphTextures = 4`, one is `heTexP3`/free) plus a dedicated mix material from id `0x4A4`, not
six separate arrays (those do not fit: `MaterialGraph.h:581`, Vulkan's `static_assert(... == 4, ...)` at
`VulkanRenderer.cpp:9492`, D3D's sampler table full per the design doc §3.1/§3.4). **Only suggested, not decided:** the foliage
`weightLayer` 6/7 reading a share as "grows where that group is."

**Traps:** a fresh Ninja+MSVC configure on this machine needs `-DCMAKE_CXX_FLAGS=/EHsc` explicitly — without it, `cl.exe`'s
default (no `/EH` flag) leaves `__cpp_exceptions` undefined and doctest's `REQUIRE`/`CHECK` macros fail a `static_assert`
project-wide (not file-specific; this project's `CMakeLists.txt` never sets `/EHsc` itself, unlike the CI workflow's exact same
`cmake -B build -G Ninja ...` invocation — root cause of that difference not chased further). The worktree's path is long enough
that `FetchContent`'s subbuild under `out/build/<preset>/_deps/...` hits Windows' 260-character `MAX_PATH` during
`nlohmann_json-populate`; configure to a short `-B` path outside the worktree instead (`C:\hb\l7` here) and override
`-DDEPLOY_DIR` so it doesn't default into the shared `C:/Users/conno/source/repos/HorizonEngineBuild`. `TerrainComponent.h`
rebuilds ~63 files; batch header edits before the first build. Background shell tasks (`run_in_background: true`) on this box
got torn down between harness turns before finishing even a 30+ minute build — ninja's own incremental state survived each
restart, but a **foreground** call with the tool's 600000 ms (10 min) cap, re-run until done, was the reliable way to drive it
to completion; `ctest` has no equivalent incremental resume, so run it in one foreground call too. No `Grep` tool in the Mac
session, use `/usr/bin/grep`; on Windows use the `Grep`/`Glob` tools or plain `grep` via Git Bash — both are available here.
