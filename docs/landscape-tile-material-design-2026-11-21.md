# Tile landscape through the material pipeline (Thema 326) — findings and plan, 2026-11-21

Status: **nothing of this is built.** Survey done on Metal, plan agreed with the reviewer. Start at "Next steps".

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

## Not verified

The SPIRV-Cross output of a patched fragment, the sampler count after the rewrite, SSAO/velocity inputs, anything on GL/Vulkan/D3D.
