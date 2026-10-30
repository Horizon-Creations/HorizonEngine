# Landscape 3.0: four texture groups on one terrain, rendering (Thema 233)

Branch `claude/landscape-3-0-multi-group-paint`, PR #213 (base `release/0.8.0`).

## What exists
* Data: `TerrainComponent::groupMix` + `baseGroup`. With it on, weight layers 8..11 (third weight page) are the
  shares of group 0..3 (`AutoLandscapeGroupId`), outside the paint layers' 255-sum. `TerrainPaint`:
  `enableGroupMix/disableGroupMix/groupShares/paintGroup/fillGroup/paintInGroup` (`TerrainPaint.cpp`, mix section).
  Scene keys `groupMix`, `baseGroup`. The first build's `mixGroups` (layers 6/7) is not read any more.
* Node `LandscapeGroupPick` (`MaterialGraph.h` v20, emitter in `emitLayerBlendNode`, `MaterialGraph.cpp`): reads the
  third page with `textureLod` (branch-pure), outputs A, B (the two strongest groups) and Mix = A's weight.
* Mix material `M_AutoLandscape_Mix` (`buildAutoLandscapeGraph(view, group, mixed=true)`, id 0x4A4): per role two bombed
  reads (slice = role + 4 * group, Groups pin 16) on one hex grid, lerped by Mix; Desert's ground = procedural sand per
  side. Tuning parameters are Mountains' for the whole terrain (decision; per-group values would cost ALU per fragment).
* Fat arrays `T_Landscape_Mix_{Albedo,Normal,Mask}_Array` (ids 0x4A5..0x4A7): `landscape_tex_gen <dir> --mix --from <dir
  with the 12 group arrays>`. They are EngineContent textures, NOT committed.
* Editor (`TerrainTools.cpp`): Setup "Mix Texture Groups"; Paint tab "Group" row + the group's layer names; the terrain
  binds the mix material while the mix is on (`bindMixMaterial`), the group's own when off.
* Witness: `HE_DUMP_AUTOLAND=1 HE_DUMP_AUTOLANDMIX=1` (Mountains centre, Forest left, Desert right, Grassland front).

## Verified
* he_tests (terrain, mix groups, landscape groups incl. mix bake on every backend variant, material graph): pass (macOS).
* Metal and OpenGL: same picture, sand blends softly into grass (`docs/images-landscape-mix-metal-2026-10-30.png`).
  Placeholder fat arrays (128 px), so only the sand is clearly distinct.

## Not verified
* D3D11, D3D12, Vulkan: variants are baked (HLSL / SPIR-V) in `test_landscape_groups`, never run on a GPU. Windows step OPEN.
* Cost: not measured. Expect ~2x the texture taps and the sand function on every fragment of a mixed terrain; the earlier
  Metal A/B (distance LOD) showed taps are cheap and ALU is what costs, so measure with `landscape_regression.py` on AC.
* Real group textures (Forest/Grassland/Desert) do not exist; the fat arrays must be built from them and published
  (EngineContent sync) before a terrain shows real group looks. Memory: 4 groups x 4 roles x 3 maps, 2K RGBA8 ~ 1 GiB.
* Gate/If layers: the mix material keeps the existing gates; effect on GPU time unknown.

## Next steps
1. Windows: build, run the witness on each RHI, compare to Metal. 2. Perf A/B (mix vs a group material). 3. Real arrays,
   BC compression. 4. Per-group tuning if wanted. 5. Setup "Texture Group" while mixed only moves `baseGroup`.
