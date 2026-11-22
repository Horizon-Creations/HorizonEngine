# Tile landscape through the material pipeline: state on 2026-11-22 (Thema 326)

Follows `docs/landscape-tile-material-design-2026-11-21.md` (design, Metal map, Vulkan / OpenGL survey). This file is the state
after the second session; where the two disagree, this one is right.

## Handoff

**State**: the tile landscape's ground is a surface like any other on Metal (material, shadows both ways, GI, SSAO, velocity,
G-buffer, fog), is drawn with its material on Vulkan in the scene pass, and builds for OpenGL where the driver has the samplers.
Branch `claude/landscape-weltmassstab-100km`, up to `1e118e0a6`.

**What each backend does** (`IRenderer::Capabilities`)

| | `landscapeMaterial` | `landscapePasses` | verified |
|---|---|---|---|
| Metal | yes | yes: shadow maps, GI pre-pass, ray structure, SSAO, velocity, G-buffer | M5, pictures + numbers |
| Vulkan | yes (scene pass, forward draw also on a deferred frame) | no | MoltenVK, pictures + numbers; frame-to-frame noise, see below |
| OpenGL | where `GL_MAX_TEXTURE_IMAGE_UNITS` >= 24 | no | NOT RUN anywhere: macOS GL 4.1 has 16 and is gated off |
| D3D11 / D3D12 | no | no | nothing built |

**What was added since the 21st** (file: function)
* Floating origin: `MaterialShaderLibrary::Lighting::worldOrigin`, `RenderWorld::worldOrigin`, filled in `FillMaterialLightWindow`
  (`LightPacking.cpp`) for every backend; the generator's World Position and world fbm add it (`MaterialGraph.cpp`, the two
  `heLight.worldOrigin` lines). The five `M_AutoLandscape*.hasset` are regenerated. Metal's G-buffer block sets it by hand
  (`EncodeGBuffer`); **other backends' reduced G-buffer fills were not looked at**.
* Metal: `EncodeLandscapePass(encoder, clip, kind, aux0, aux1)` kinds 0-4, `EncodeLandscapeGBuffer`, `BindLandscapeMaterial`,
  `m_giLandMesh` in `EncodeGIAccelBuild`; `kLandscapeMSL`: `lsWorldRel`, `vertexVelocity`, `fragmentVelocity`, `fragmentViewPos`,
  `fragmentViewPosMrt`, `fragmentGiGBuf`, `LsGlobals::aux0/aux1`.
* GI colour: `RenderWorld::tileLandscapeIndex` (an entry of `landscapes` for an Auto Landscape material, `RenderExtractor.cpp`).
* Vulkan: `m_lsMatSetLayout` / `m_lsMatPipelineLayout` (`createMaterialResources`), `GetOrBuildLandscapeMaterialPipeline`,
  `lsDraw` hooks in `drawDCVk`, `VulkanLandscape::materialImages` / `drawPatches`. SPIR-V form of the shaders: images without a
  sampler (`landscapeSeparateImages`), `LsPatch` as push constant.
* OpenGL: `GetOrBuildLandscapeMaterialProgram`, `OpenGLLandscape::Shading::materialProgram`; the landscape unbinds its units.
* Speed: slot looked up once a fragment (`heLsFind`), one filtered read on Metal (`HE_LS_FILTERED`), no discard (vertex clamp),
  `heLsWeightOnePage` reads nothing.
* Tools: tile brush Roughen, Ramp (`TerrainSculpt::ramp`), Mountain (`TerrainGenerate::mountain`).
* `landscape_gpu_check --timing`, `HE_LANDSCAPE_PASSES=0`.

**Verified** (Apple M5; `he_tests` 6509/6509; `landscape_gpu_check --arrays EditorDeps/EngineContent` exit 0)
* Metal, forward and deferred: marker material 112 073 of 129 600 px; box shadow 4 686 px; ground casts 150 vs 223/255; GI on:
  shadow 4 907 px, ground casts 113 vs 218/255; SSAO 26 739 px; plane does not shadow itself (60 of 112 245 px); fog 66.7/255;
  Auto Landscape through `LandscapeWorldComponent` + `update(world)`; origin shift 0.05/255 (was 22.8).
* Vulkan (MoltenVK): marker 113 523 px, Auto Landscape draws, fog, origin 0.05/255, deferred picture equals Metal's.
* Frame times, 1600 x 900, median of 60 with read-back: fixed blend 5.8 ms; material 14.1 (35.2 before the three fixes); material
  in the scene pass alone 13.0; a `TerrainComponent` landscape of 2 x 2 km with the same material 11.1.

**Not verified**
* The ground's velocity (TAA) and the ray-traced reflection of the ground: both pictures come out equal with and without, the
  capture path does not show them. Built, unproven.
* Vulkan forward on MoltenVK draws the BOX black, with or without the material path (picture `..._fixedblend.png`), and its
  shadow and origin numbers vary between runs (origin 0.05 to 2.9). Not looked into; real Vulkan on NN-WS03 should say.
* Any OpenGL run, D3D, Windows, the editor by hand, an export.

**Known gaps**
* The baked far image is not read while a material draws; RVT: the ground reads no virtual texture. Both cost time, not correctness.
* GI: 2 km of clipmap at half resolution in the ray structure; SW ray path without it; no terrain entry for non-Auto materials.
* Vulkan / OpenGL: no `landscapePasses`. D3D: nothing.
* Tools on tiles still missing: foliage density mask, water brush and lakes, heightmap import onto an existing world.
* The first frame with a new material compiles on the render thread.

**Next steps**
1. Far image photographed from the material, read by the variant for ground outside the atlas.
2. RVT for the tile ground.
3. `landscapePasses` on Vulkan (shadow layers `drawDepthLayer`, GI raster in `runGi`, G-buffer), then D3D.
4. Foliage mask, water, import on tiles.
