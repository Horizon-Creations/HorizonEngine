# Volumetric fog on Vulkan (Thema 225, step 5), 2026-11-02

Plan: `docs/volumetric-fog-plan.md`. Metal: `docs/volumetric-fog-metal-2026-11-02.md`. The step was stopped once by the queen and then finished (see Verified).

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
- `VulkanRenderer.cpp` passes `c++ -fsyntax-only -Wall -Wextra` against MoltenVK 1.4.1 (negative control without the include fails); MSVC compiled it in the Windows CI job without a Vulkan error.
- The three kernels (real preamble text) compile with `glslangValidator -V` and pass `spirv-val`.
- `he_tests` (macOS, full build): `*test_fog_volume.cpp` 15/15 (incl. the 2 new Vulkan cases), `*test_material_graph.cpp` 109/109, `*test_engine_materials.cpp` 15/15 (the Water `samplers <= 16` check still holds).
- **lavapipe (CI run 41529249343, job "Linux · Vulkan (lavapipe)"): `he_vk_imagetests` 16/16 cases passed, validation layer on.** Case `fog`: m0/m1/m1_parity/m1_def all exit 0, `validation(dump)=0`, log witness "volumetric fog frame (grid 96x54x32 ...)" present at fogMode 1 and absent at 0. m0 vs m1 mean |Δ| 20.15 (99.9 % of pixels changed, max 63), so the volume reaches the frame; m1 vs m1_def (forward vs deferred) mean |Δ| 18.3 and m0 vs m1_parity 8.4: report-only, not judged. All 15 other cases (mode 0) kept their thresholds, which is the mode-0 witness (not a bit-exact A/B).

## NOT verified
- Real Vulkan GPU (RTX 4070 / NN-WS03): only lavapipe ran it.
- The shaft look. I opened fog_m0/m1/m1_def.bmp (artifact `vulkan-lavapipe-shots`): m1 is plainly fogged (sky and wall hazed toward the sun-side glow, horizon band in the right place, not mirrored), but the FOGTEST camera ends up so close to the wall that the slits are only 2-3 thin gaps and no shafts can be read. The scene needs a farther camera / wider slits before this case can show witness (a). m1 vs m1_def differ by 18 steps (deferred is lighter, greyer; the ground colour differs too): I could not tell fog from the forward/deferred lighting difference without an m0_def shot; Metal had 1 LSB, so compare deferred m0 vs m1 before calling the deferred Apply split right.
- A bit-exact mode-0 A/B against `release/0.8.0`; `HE_HAVE_SHADERC=OFF` Vulkan builds; TAA with fog on.
- CI run 41531572019 on the fixed commit: Windows, macOS and the lavapipe job green; the Linux job was still running when I closed (it was green in run 41529249343 before the test-only fix).

## Decisions
- Binding 34 is always in the layout (translucent SPIR-V uses it statically); a device below 18 fragment samplers drops it (and compiles without `HE_FOG_VOLUME`) before it drops the weightmap row; fog is then off.
- Fog volumes stay in `GENERAL` for good; inject/integrate run outside any render pass, so a second `Lighting` fill is built in `encodeFogVolume` (DrawScene's lives inside the pass and zeroes the CSM on GI frames).
- Dummy 3D lives with the material resources, not with the fog pipelines (else a failed fog build leaves a layout row nothing can bind).

## Next steps
1. Move the FOGTEST camera back / widen the slits in `he_vk_imagetests.py` (case `fog`) so shafts are visible, add an `m0_def` variant, then judge shafts and the forward/deferred gap.
2. Tighten the `fog` thresholds from the printed numbers (m0 vs m1 20.15 -> a floor near 10; consider a parity bound once m1_parity is understood).
3. Step 6 (D3D) can copy the table approach; step 7: weather hooks, default `fogMode`.
