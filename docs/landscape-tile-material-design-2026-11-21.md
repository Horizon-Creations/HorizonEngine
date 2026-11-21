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
* GI on (Metal): the ground is NOT in the ray-tracing acceleration structure, and the cascades are off while GI owns the sun, so with
  GI on the ground casts no shadow (mountains do not shadow valleys) and bounces no light. It receives. Needs the GI height clipmap
  (`extractLandscapeGiHeightfield`) as a mesh in the TLAS.
* The shadow pass has no fragment stage, so the top node's ground beyond the world's edge casts too.
* SSAO and velocity pre-passes do not have the ground.
* The first frame with a new material compiles on the render thread (one hitch).
* RVT: the ground reads no virtual texture (no `TerrainComponent`, so `RvtLandscape` paints nothing for it); the material evaluates inline.
* The baked far image is not read while a material draws (the material runs everywhere, so there is no ring); photographing the
  material into it would be a saving, not a fix.
* GL 4.1 / Vulkan / D3D: fixed blend. Windows: nothing of this ran.
* Editor by hand: not opened.

**Next steps**
1. Terrain into the Metal TLAS (GI on: ground casts and bounces).
2. Vulkan: descriptor bindings 50-60 in the material layout, draw in the scene pass, landscape in shadow and G-buffer passes.
3. GL: same (4.1 has no binding qualifiers above the unit count: units are assigned by uniform name).
4. D3D11/12: registers for 50-60 (SM 5.0 has none free in the material fragment: weights must go through `Load` on t-registers only).
5. Time it (`he_perf_capture.py`), then decide on the far image from the material.
6. Tools on tiles: Ramp, Mountain, foliage mask, water, heightmap import.

## Not verified at design time (kept for the record)

The SPIRV-Cross output of a patched fragment, the sampler count after the rewrite, SSAO/velocity inputs, anything on GL/Vulkan/D3D.
