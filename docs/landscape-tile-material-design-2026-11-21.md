# Tile landscape through the material pipeline (Thema 326) — findings and plan, 2026-11-21

Status: **built and verified on Metal** (steps 1-6 below, see Handoff at the end). GL, Vulkan and D3D still draw the fixed blend.

## What is true today (verified by reading, Metal)

* `kLandscapeFS` / `kLandscapeMSL` is a fixed layer blend with its own sun + ambient. It is not the Auto Landscape material and reads
  no shadow, fog or GI input.
* The tile landscape is drawn in ONE place: `MetalRenderer::EncodeScene` -> `EncodeLandscape` (MetalRenderer.mm:16315, call at :17136),
  forward, after the skinned objects. It is in no other pass: not `EncodeShadowMap` (:8561, early return on empty objects at :8586), not
  the GI G-buffer pre-pass of `EncodeGIShadowRays` (:9511, rasterises `m_renderWorld` objects only), not `EncodeSSAO`, not `EncodeGBuffer`
  (:18586), not velocity.
* With GI on, the sun shadow of a pixel comes from that GI pre-pass (position + normal). A forward material fragment on landscape pixels
  therefore gets NO sun shadow until the landscape is rasterised there as well. Forward alone is not enough.
* Material fragments (`MaterialGraph.cpp` header :2640) read four varyings: `vNormal` 0, `vColor` 1, `vUV` 2, `vWorldPos` 3. Their tail is
  `heApplyFog(heLitP(...))`: shadows, GI, fog come with the fragment once its inputs are there.
* Layer weights: ONE 2D `heLandscapeWeights` (GLSL binding 14, Metal texture 13), pages side by side, `vUV` = 0..1 over the landscape
  (`MaterialGraph.cpp:1577-1594`, group pick :1641). The tile atlas is per-tile array slices behind a slot table: it does not fit.
* Metal fragment slots: samplers 0-15 all taken, textures 0-24 taken (18-24 = RVT). Free: textures 25-30, and 13 if the weightmap goes.
  `MetalLandscape::encode` binds fragment 20-26 today, which collides with RVT.
* The vertex-stage pattern to copy is the UI quad: `kUIVertex` + `MaterialShaderLibrary::uiVertex` (MaterialShaderLibrary.cpp:1538-1590)
  and `GetOrBuildUIMaterialPipeline` (MetalRenderer.mm:15556).
* `LandscapeWorldComponent` has no material field. `LandscapeRuntime::buildRenderFrame` consumes the frame's uploads, so a second call
  per frame (shadow views) must not go through the provider as it is.
* GI occlusion by the tile ground exists already (`extractLandscapeGiHeightfield`, RenderExtractor.cpp:800).

## Decisions

1. Landscape patch vertex stage in GLSL (from `kLandscapeVS`), writing the four varyings, normal from height differences in the vertex.
2. The weight reads are rewritten when the landscape VARIANT of a fragment is built (`textureSize/texture/textureLod(heLandscapeWeights`
   -> functions that go through slot table, tile pages, coarse pages, `texelFetch` only). Never in the generator: its text is baked.
   New textures on Metal: 13 and 25-30, inline samplers.
3. `vUV` = position in the render window, 0..1.
4. The landscape becomes a geometry kind of every pass, not a forward extra: CSM depth, GI pre-pass (position + normal), SSAO, main.
5. Shadow views take the main frame's patches (same LOD as the receiver) but need their own cull: a provider option, not a second frame.
6. D3D behind a capability switch, as with the window move.

## Next steps (in order, commit each)

1. `MaterialShaderLibrary::landscapeVertex(Backend)` + test that it compiles for Metal/GL/SPIR-V.
2. `landscapeFragment(key, glsl, clustered)`: the rewrite; tests: no `heLandscapeWeights` left, the unpatched Auto Landscape text and key
   byte-identical to before.
3. `LandscapeWorldComponent::material` (serializer, old-form load test), `LandscapeRenderStatic::materialId`.
4. Metal: pipeline cache like the UI one (clustered and not), draw in `EncodeLandscape`; fixed blend while pending.
5. Metal: landscape in `EncodeShadowMap` and in the GI pre-pass; handle the empty-objects early returns.
6. `landscape_gpu_check` scenes: marker material vs fixed blend, caster on/off (GI on and off), fog on/off.
7. Decide what the landscape draws for `rvtSlot >= 0` captures, then the far painter photographs the material (`HlodBaker::renderView`).
8. GL, Vulkan; D3D into the Windows handoff.
9. Tools on tiles: Ramp, Roughen, Mountain, foliage mask, water, heightmap import (today: Raise, Lower, Smooth, Flatten, Paint Layer).

## Handoff

**State**: on Metal the tile landscape is drawn with its material, takes shadows (cascades and ray-traced), casts into the shadow maps,
is fogged. Branch `claude/landscape-weltmassstab-100km`, commits `b8b5c66a5`, `d087290f2`, `57fc9a755`, `08e903a87`.

**What exists**
* `MaterialShaderLibrary::landscapeVertex / landscapeFragment / landscapeVertexSource / landscapeFragmentSource`
  (`src/HE_Rendering/src/material/MaterialShaderLibrary.cpp`, behind `uiVertex`). GLSL bindings: 50 LsGlobals (both stages), 51 slot table,
  52-54 tile weight pages, 55-57 coarse weight pages, 58 LsPatch, 59 tile heights, 60 coarse heights. Metal pins: vertex buffers 20/21,
  vertex textures 20-22; fragment buffer 20, textures 13 and 25-30, all with inline samplers.
* `LandscapeRenderFrame::materialId` (`src/HE_Core/include/Renderer/LandscapeRenderFrame.h`): the landscape entity's
  `MaterialComponent`, else the texture group's Auto Landscape material (`LandscapeRuntime::update`); `setMaterialId` for tools.
  No serializer change: `MaterialComponent` is the field.
* `IRenderer::Capabilities::landscapeMaterial` (Metal only).
* `MetalRenderer::GetOrBuildLandscapeMaterialPipeline`, `EnsureLandscape`, `EncodeLandscapePass` (shadow layers, GI pre-pass),
  `MetalLandscape::Shading::materialPipeline / passPipeline`, `MetalLandscape::makePassPipeline`, `fragmentGiGBuf` in `kLandscapeMSL`.
* `HE_LANDSCAPE_PASSES=0`: the ground in the scene pass alone (A/B switch, read per call).
* Editor: tile brush has Roughen.

**Verified** (Apple M5, `landscape_gpu_check --arrays EditorDeps/EngineContent`, exit 0; `he_tests` 6508/6508 before the last two commits,
`tile landscape variant*` test after)
* marker material: 112 020 of 129 600 px are its layer 0; without the material 0.8 % blue, the fixed green.
* box shadow on the ground: 4 686 px darker (GI off); with GI on 4 907 px darker than with the ground left out of the pre-pass.
* the ground casts: a box under the ground 150/255 against 223/255 without the ground in the shadow maps.
* fog moves the ground by 66.7/255. Same numbers forward and deferred.
* the engine's Auto Landscape (Forest) material draws the ground: picture in `docs/landscape-tile-material-2026-11-21/`.

* through the scene: a `LandscapeWorldComponent` (texture group Forest, no material of its own) and `LandscapeRuntime::update(world)`
  draw the Auto Landscape material; the renderer loads the group's material from `LandscapeRenderFrame::materialPath` when nothing has.

**Not verified / known gaps**
* **Floating origin: the material's texturing moves when the origin is rebased** (measured: 22.8/255 mean difference for the same
  absolute camera after an origin shift). A graph material's World Position is origin-relative (`vWorldPos`, every material, every mesh,
  the old terrain too); the fixed blend has `uvOrigin` for this. It shows once per rebase, and only with
  `ProjectSettings::physics.floatingOriginRadius` > 0, which a world over ~32 km needs. The fix is in the generator, not here: a position
  node that adds the origin (a new `HeLighting` member the backends fill), used by the Auto Landscape graph for its texturing, with the
  engine content regenerated. `WorldPos` itself must stay relative (Camera Distance, RVT and lighting use it).
* No timing at all. The vertex stage reads up to 11 heights a vertex, the far ground runs the whole material.
* GI on (Metal): the ground IS in the ray-tracing structure since `e4857a0c0` (the GI height clipmap as a mesh, `m_giLandMesh` in
  `EncodeGIAccelBuild`; verified: a box under the ground 113/255 against 218/255 without). Limits: the 2 km of the clipmap only, every
  second height (4 m) where the ground is drawn at 2 m, so shadow acne on rough ground is possible and was only seen on a plane; the
  bounce colour is one constant, not the material; the software ray path (`EncodeGISwAccelBuild`) does not have it; a scene with no
  mesh at all builds no structure.
* The shadow pass has no fragment stage, so the top node's ground beyond the world's edge casts too.
* SSAO and velocity pre-passes do not have the ground.
* The first frame with a new material compiles on the render thread (one hitch).
* RVT: the ground reads no virtual texture (no `TerrainComponent`, so `RvtLandscape` paints nothing for it); the material evaluates inline.
* The baked far image is not read while a material draws (the material runs everywhere, so there is no ring); photographing the
  material into it would be a saving, not a fix.
* GL 4.1 / Vulkan / D3D: fixed blend. Windows: nothing of this ran.
* Editor by hand: not opened.

**Vulkan: surveyed, not built** (VR = `src/HE_Rendering/src/Backends/Vulkan/VulkanRenderer.cpp`, VL = `VulkanLandscape.cpp`)
* Material pipelines: `GetOrBuildMaterialPipeline` VR:3402 (key has no landscape bit; vertex hard-wired to standard/custom VR:3520;
  refuses a fragment with `heLandscapeWeights` when `!m_matLandscapeWeights` VR:3442). One set, `m_matSetLayout` from
  `vkmat::kBindings` (`VulkanMaterialLayout.h:92`), built in `createMaterialResources` VR:3072; `m_matPipelineLayout` has NO push
  constants. The sampler fit counts layout ROWS, with `static_assert`s (`VulkanMaterialLayout.h:193-260`) and
  `kPreClusterBindingCount == 23` (VR:10970): do not add 50-60 to `kBindings`, make a second layout for the landscape (the material's
  rows plus 50-60) with its own pipeline layout.
* A material draw's set is written in the `bindMatSlot` lambda (VR:10792-11075) inside `drawDCVk` (VR:10610), per-frame cache by a
  hash of the writes; three dynamic offsets (bindings 1/3/9). The landscape needs the same writes plus its own.
* `VulkanLandscape` exposes nothing (images, views, samplers, vbo, UINT16 ibo, UBO are private in `Impl`, VL:33-60), writes ONE
  LsGlobals UBO and ONE set per frame inside `draw()` (VL:538-577) and pushes `PatchPush` as a push constant (VL:593). More than one
  landscape draw a frame (shadow layers, GI pre-pass, scene) needs LsGlobals per pass. `LsPatch` is a uniform block at 58 in the
  material's vertex stage: easiest is a SPIR-V form of `landscapeVertexSource` with `layout(push_constant)`.
* Call sites: `landscapeBegin` VR:10046 (provider asked once, `zeroToOne = true`, `kVulkanClipFix` applied), `landscapeDraw` VR:10087,
  called at VR:10170 and VR:10181 (early returns, BEFORE the HeLighting fill and pool reset at VR:10347: a material draw there reads
  last frame's lighting) and VR:11485. Shadow layers: `drawDepthLayer` VR:8643, early return on no objects VR:8589. GI pre-pass:
  `runGi` VR:14322, raster at VR:14529-14583, early returns VR:14343-14367.
* R16_UINT images (51, 59, 60) need the landscape's NEAREST sampler.

**OpenGL: surveyed, not built** (GLR = `OpenGLRenderer.cpp`)
* `GetOrBuildMaterialProgram` GLR:3992, GLSL410 on the Mac; units by name in `setupProgram` GLR:4101-4166; `DrawLandscape` GLR:12322.
* Binding 51 has two names (`uSlotTable` in the vertex, `heLsSlotTable` in the fragment): each needs a unit.
* **Possible existing fault**: `OpenGLLandscape::kFirstUnit = 21` (units 21-33) overlaps the materials' 21 (fog volume) and 22-25 (RVT).
  Not looked into: whether every material draw after the landscape rebinds those units.

**Next steps**
1. Vulkan as above (MoltenVK on the Mac can run `landscape_gpu_check --only vulkan`), then OpenGL.
2. The floating origin (above): the generator's position node.
3. Mountain on the tile brush (the Landscape mode's is a generator with its own drag state, `TerrainTools.cpp:703`); Ramp and Roughen
   are there (`TerrainSculpt::ramp`).
4. D3D11/12: registers for 50-60 (SM 5.0 has none free in the material fragment: weights must go through `Load` on t-registers only).
5. Time it (`he_perf_capture.py`), then decide on the far image from the material.
6. Tools on tiles still missing: foliage mask, water, heightmap import onto an existing world.

## Not verified at design time (kept for the record)

The SPIRV-Cross output of a patched fragment, the sampler count after the rewrite, SSAO/velocity inputs, anything on GL/Vulkan/D3D.
