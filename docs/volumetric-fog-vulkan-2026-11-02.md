# Volumetric fog on Vulkan (Thema 225, step 5), 2026-11-02

Plan: `docs/volumetric-fog-plan.md`. Metal: `docs/volumetric-fog-metal-2026-11-02.md`. The step was **stopped by the queen
before it was finished**; this is the state at that moment. Nothing here has ever run on a Vulkan device.

## What is in place (all in the working tree commit, see the Hive post)

| Piece | Where |
|---|---|
| SPIR-V kernels: `fogInject/fogIntegrate/fogApply(Backend::SpirV)` (GLSL 450 as it stands, compute + fragment, canonical bindings, no pins) | `MaterialShaderLibrary.cpp` (grep `Backend::SpirV` near `fogInject`) |
| `HE_FOG_VOLUME` for SpirV via instance switch `setSpirvFogVolume(bool)` (default on) | `MaterialShaderLibrary.{h,cpp}` (`fragmentVariant`) |
| Material set row 34 `heFogVolume` (+ fog descriptor tables `kFogInject/Integrate/ApplyBindings`, `fogVolumeFit`, `fragmentSamplerCount(weights, fog)`) | `VulkanMaterialLayout.h` |
| `createFogPipelines` (eager at Initialize, sets `supportsVolumetricFog`), `createFogDummy` (1x1x1 3D at binding 34, owned by the material resources), `ensureFogTargets`, `encodeFogVolume` (Inject+Integrate, before the scene pass), `encodeFogApplyAndReopen` (depth flip, Apply onto `m_hdrView` via `m_underwaterFogRP/FB`, reopen `m_hdrLoadRP`) | `VulkanRenderer.cpp` (end of file; split call in `DrawScene` before `transPipe`; call in `DrawViewportFrame` before `hdrpbi`) |
| Material fill: `fogVol/fogVolGrid/fogViewProj`; built-in frame UBO fog density 0 when the volume is active; binding 34 write | `VulkanRenderer.cpp` (grep `m_fogFrameActive`) |
| Tests: SPIR-V descriptor reflection of the 3 kernels against the tables, binding 34 in the material SPIR-V, old "SpirV stays off" asserts flipped | `tests/test_fog_volume.cpp`, `tests/test_material_graph.cpp:~5074` |
| lavapipe image case `fog` (m0 / m1 / m1_parity / m1_def) | `scripts/he_vk_imagetests.py` |

Clip conventions (derived, written into the block comment above `createFogImage3D`): kernels get `inverse(kZeroToOneDepthClipFix*P*V)` (= flipY*kVulkanClipFix),
previous frame the same form; `fogViewProj` for `heApplyFogT` is the plain `kVulkanClipFix*P*V`; depth needs no remap.
`supportsVolumetricFog = m_fogReady && m_postFxReady`; `m_fogReady` only if every step of `createFogPipelines` succeeded.

## Verified
- `VulkanRenderer.cpp` passes `c++ -fsyntax-only -Wall -Wextra` against MoltenVK 1.4.1 headers (negative control without the include fails on `vulkan/vulkan.h`).
- The three kernels, assembled from the real preamble text, compile with `glslangValidator -V` and pass `spirv-val` (inject.comp, integrate.comp, apply.frag). spirv-cross reflection lists the 13 preamble samplers + 35/36/37/38 for inject, 37/38/36 for integrate, 15/34/36/39 for apply: the tables match that.
- `he_tests` build was at 774/1052 with 0 compile errors when stopped.

## NOT verified
- The `he_tests` run (`*test_fog_volume.cpp`, `*test_material_graph.cpp`, `*test_engine_materials.cpp`) never ran. Expect to check: `test_engine_materials.cpp:~1245` (`samplers <= 16` for the Water module now has binding 34 as one more sampler; if it fails, assert 16 on a library with `setSpirvFogVolume(false)` and 17 on the default); the exact set of statically used inject bindings (the test requires coverage, not equality).
- Nothing ran on any Vulkan device (this Mac has none): all barriers, layouts, the Apply render-pass split, validation cleanliness, the shaft look, mode-0 pixel identity. The lavapipe `fog` case is the first execution; its mean-|Δ| floor 0.5 is a guess. CI was not started.
- `HE_HAVE_SHADERC=OFF` Vulkan builds (the code is `#if`-guarded; not built).

## Decisions
- Binding 34 is always in the layout (translucent SPIR-V uses it statically); a device below 18 fragment samplers drops it (and compiles without `HE_FOG_VOLUME`) before it drops the weightmap row; fog is then off.
- Fog volumes stay in `GENERAL` for good; inject/integrate run outside any render pass, so a second `Lighting` fill is built in `encodeFogVolume` (DrawScene's lives inside the pass and zeroes the CSM on GI frames).
- Dummy 3D lives with the material resources, not with the fog pipelines (else a failed fog build leaves a layout row nothing can bind).

## Next steps
1. `cmake --build cmake-build-release --target he_tests -j 8` to the end, then `he_tests --source-file=*test_fog_volume.cpp` and the two material test files; fix the Water sampler bound if needed.
2. Push, `gh workflow run CI --ref <branch>`; read job `vulkan-lavapipe` case `fog` (validation messages, the four BMPs in the `vulkan-lavapipe-shots` artifact: shafts in m1, m0 unchanged).
3. Step 6 (D3D) can copy the table approach; step 7: weather hooks, default `fogMode`.
