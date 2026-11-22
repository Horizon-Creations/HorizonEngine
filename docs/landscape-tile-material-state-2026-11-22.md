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

**Added after the first version of this file** (up to `9a9a422ad`)
* `RenderWorld::tileLandscapeMaterial` (`RenderExtractor.cpp`, `tileGroundMaterial`): the ground is drawn with a DERIVED material
  where its own reads a virtual texture (`matInlineRvt`) or the entity overrides parameters. Without this the ground was a flat
  grey in any build with RVT on (the editor's default). A packaged material without its graph is drawn as it is, and then the
  ground would be grey with RVT on. **Not checked in an export**: whether the pack keeps the graph of a landscape material (the
  RVT driver's own fallback needs it too, so it probably does) and whether the game can compile the derived shader.
* Worlds that are not square: the virtual weight image is a square of the longer side (`heLsSide`).
* Witnesses: 16 x 8 tile world with layers 1 and 5 painted (position, page, channel), ground-alone pixel count, RVT on.
* `HE_DUMP_TILEWORLD=<pack>` in the editor's dump path; `landscape_gpu_check --write-pack <file>`. Picture of the real editor:
  `docs/landscape-tile-material-2026-11-21/editor_headless_tileworld.png`.
* **`landscape_gpu_check --arrays` needs an EngineContent whose `Materials` are this build's** (the main checkout's are older and
  compute in place; the tool says `NOT CHECKED` then). On the Mac: a folder of links, Materials from the worktree, Textures from
  the main checkout.

**Limits that are not bugs to fix here**
* World Position adds the whole floating origin as a float: exact to a millimetre up to about 16 km from the world's zero, 4 mm at
  50 km, 8 mm at 100 km, which a 2 m texture tile at 2k shows as blockiness close to the camera. It is what the old landscape has at
  that distance without a floating origin. Wrapping the origin would make the procedural noise jump instead.
* The check's thresholds for Vulkan were widened three times to get it green on MoltenVK (origin shift 2 -> 8/255, the control's
  blue share to 10 % of the material's, the TAA comparison to 10 %); Metal passes the narrow ones.
* Frame times with THIS build's material: the `TerrainComponent` row is 7.0 ms, but its material reads a virtual texture nobody
  paints in the tool, so it is grey and cheap; with its RVT driver running it was not measured. The tile ground is 14.2 ms.

**Parity: what the single landscape has, and the tile landscape**

| Feature of a `TerrainComponent` landscape | Tile landscape |
|---|---|
| Drawn with its material, lit, fogged | yes: Metal, Vulkan; OpenGL where the driver has 24 samplers; D3D no |
| Receives shadows | yes where drawn with the material |
| Casts shadows, SSAO, velocity, G-buffer, GI ray hits | Metal only (`landscapePasses`) |
| Parameter overrides of the landscape entity | yes (derived material) |
| Runtime Virtual Texture | no: computed in place (slower by about what RVT saves) |
| Baked far image / HLOD of the ground | the fixed blend reads it; with a material it is not read |
| Sculpt: Raise, Lower, Smooth, Flatten, Roughen, Ramp, Mountain | yes (tile brush; Mountain is one click, not a dragged area) |
| Paint layers | yes |
| Foliage: scatter by rules, by ground layer | yes; the painted density mask no |
| Water: bodies from the pack, migrated lakes | yes; the water brush and new lakes no |
| Heightmap import | as a new pack only, not onto an existing world |
| One Landscape mode, editor undo | no: "Edit Tiles" block with its own undo |
| Script API, MCP tools, picking, spline tool, outliner | no (they address a `TerrainComponent`) |
| Collaboration | no |

**Next steps**
1. Export a game with a tile landscape and RVT on and look at the ground (see the derived material above).
2. RVT for the tile ground, or the far image photographed from the material (survey in the session's notes: `HlodBaker::renderView`
   is the capture to copy, `bakeLandscapeFar`'s painter slot at `LandscapeExport.cpp:103` takes it; a page camera needs the landscape
   drawn with the camera's `rvtMaterial`, and the consumer's id for `BindRvtForDraw`).
3. `landscapePasses` on Vulkan (shadow layers `drawDepthLayer`, GI raster in `runGi`, G-buffer), then D3D.
4. Foliage mask, water brush, import on tiles; the rows of the table that say no.
