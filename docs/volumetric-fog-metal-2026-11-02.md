# Volumetric fog on Metal (Thema 225, step 3), 2026-11-02

Plan: `docs/volumetric-fog-plan.md` (§5 Inject, §6 Integrate/Apply/`heApplyFogT`, §7 seam, §8 budget, §9 witnesses,
§10 shaders). This step runs the plan end to end on Metal with `fogMode = 1`; with `fogMode = 0` the frame is unchanged.
Witness images (JPEG, 1280x720): `docs/volumetric-fog-metal-2026-11-02/`.

## What runs where (Metal, `fogMode = 1`)

| Pass | Code | Notes |
|---|---|---|
| Inject (compute, 8x4x2 threads) | `MetalRenderer::EncodeFogVolume`, kernel `kInjectMain` in `src/HE_Rendering/src/material/FogVolumeShaderSource.h` | after shadow maps / cloud shadow / sky-view LUT, before the scene encoder. Reuses the REAL `kLightingPreamble` (`heCsmShadow`, `heLocalShadowFactor`, `heCloudShadowFactor`) |
| Integrate (compute, one thread per column) | same, `kIntegrateMain` | SRV `texelFetch` + storage-image store only, no typed UAV load |
| Apply (full-screen, blend ONE / SRC_ALPHA, alpha ZERO / ONE) | `EncodeFogApplyAndReopen`, `kApplyMain` | `EncodeScene` ends the scene encoder after the sky, runs Apply on `m_hdrColor` (depth as texture 2), reopens the encoder with colour+depth Load for the translucent tail + particles. `hdrPass` depth is stored while the fog runs |
| `heApplyFogT` | preamble in `MaterialShaderLibrary.cpp` (search `heApplyFogT`), emitted by `MaterialGraph.cpp` for `MatBlendMode::Translucent` | reads the integrated volume with 8 `texelFetch` (trilinear by hand) |

Kernels are `MaterialShaderLibrary::fogInject/fogIntegrate/fogApply(Backend)` (one GLSL source, glslang -> SPIRV-Cross -> MSL
through the pin path; Metal only, other backends return `ok == false` with a "not yet" log).

**Binding 34 on Metal = fragment TEXTURE 17** (`MaterialShaderLibrary::kMetalFogVolumeTextureIndex`), read through an inline
constexpr sampler (`kMaterialMslOptions` got `{0, 34}`), so no sampler slot. `BindMaterialPreambleSlots` binds the
integrated volume (fog on) or a 1x1x1 3D dummy (Metal validates the texture type). Kernel-side pins (own index spaces):
inject buffers HeLighting 1 / HeFogVol 0, CSM 11, local atlas 12, sky env 14, cloud shadow tex 16, noise 3, history 4,
output image 5; integrate HeFogVol 0, injected 0, integrated image 1; apply HeFogVol 0, volume 0, sky env 1, depth 2.

## Handoff

- **Topic / step:** 225 (echtes volumetrisches Nebel), step 3 of 7
- **Branch / commit:** `claude/echtes-volumetrisches-nebel-statt-hintergrund-blend`, commits `b5ee7e4c`, `b0275bbb` and the final one (pushed: see Hive post)
- **Done:** Metal runs Inject, Integrate and Apply with `fogMode = 1` (forward AND deferred, identical to 1 LSB); translucent graph materials
  (water) read the volume via `heApplyFogT`; `fogMode = 0` is unchanged; `RenderExtractor` wires `ExtractFogVolumeData`; Metal reports
  `supportsVolumetricFog`.
- **Files touched:** `FogVolumeShaderSource.h` (new, kernels), `MaterialShaderLibrary.{h,cpp}` (Lighting v3.6 `fogVol/fogVolGrid/fogViewProj`,
  `heApplyFogAnalytic/heApplyFog/heApplyFogT`, `HE_FOG_VOLUME` define for Metal material variants only, `HE_SHADOW_PCF_R`, kernel builders),
  `MaterialGraph.cpp:1944` (emits `heApplyFogT` for Translucent), `MetalRenderer.{h,mm}` (grep `Fog`), `RenderExtractor.{h,cpp}`
  (`setVolumetricFogCapable`, `out.fogVolume`), `FogVolume.h` (env debug switches, gains), `EditorApplication.cpp` (HE_DUMP_FOG* keys,
  `HE_DUMP_FOGTEST`), `tests/test_fog_volume.cpp`.
- **Verified:**
  - `he_tests`: `*test_fog_volume.cpp` 12/12 (new: kernels cross-compile for Metal with the expected pins, texture 17 on Metal only,
    other backends' fragments contain no `heFogVolume`), `*test_material_graph.cpp` 103/103 (incl. the register test),
    `*test_scene_serializer.cpp` 48/48. Full run: 5465/5468; the 3 failures are `test_profiler` (2 checks, passes alone) and `test_hpak.cpp:1188`
    (`scanContentDirectory() == 7`, fails alone too; it scans the SHARED `$TMPDIR/he_registry` while another bee's `he_tests` was running: environment
    collision, not touched by this change, not proven against a clean `main` build).
  - Mode-0 A/B (same editor flags, `HE_SKY_TIME=10`, AA off, RENDERPATH pinned, 1280x720, `HE_DUMP_FOGTEST` scene and `HE_DUMP_WATERTEST=floor`
    with fog density 0.03; forward and deferred): new vs the saved pre-change editor: max 1/255 on 0.8..12 % of the bytes, the same as the control
    (same binary twice: 0.8..9.8 %), 0 bytes differ by more than 1. The run is NOT bit-exact even for identical binaries (1 LSB dither noise).
  - Parity (`HE_FOG_PARITY=1`, plan §9): water scene (translucent graph material + grey slab + sky), mode 0 vs mode 1: median 0, p99 1, max 2 of 255
    over the whole frame (1/4-sampled). That includes `heApplyFogT` reading texture 17.
  - Witnesses (`docs/volumetric-fog-metal-2026-11-02/`): (a) `a_shafts_m1.jpg` (+ `_m0`, `_m1_parity`): five pillars, low sun to the left, camera at (0,3,-24)
    looking +z: fog lit on the sun side, in shadow behind the pillars; sun term (m1 minus parity) mean luma +47 in the lit lane vs +12 right of the last
    pillar (this includes the phase falloff, so it is not a pure shadow number). Shafts are soft, not crisp wedges: the sky ambient term dominates;
    (b) `b_bank_m1.jpg` vs `_m0`: 4 m ground bank over water, camera 7 m up: the water (translucent, `heApplyFogT`) is veiled, mode 0 shows clear water;
    (c) `c_point_m1.jpg` (+ `_nolights`, `_m0`): night, shadow-casting point lamp in the fog: warm halo; `HE_DUMP_FOGLIGHTS=0` removes it;
    (d) underwater (`WATERLAKE=bowl`, camera 2 m under): mode 1 == mode 0 (max 1 LSB) and the fog pipelines are never even built (log), i.e. the volume is off.
  - Deferred vs forward in mode 1: max 1 LSB.
  - Cost (see below).
- **Not verified:** a bit-exact mode-0 A/B (impossible, the noise floor is 1 LSB); `MTL_DEBUG_LAYER=1` (the 3D dummy / 17 binding was not run under the
  validation layer); other Apple GPUs; Intel/AMD Macs; TAA/MetalFX with the fog on (Apply reads the unjittered inverse view-projection and the TAA-jittered
  depth, as the underwater pass does; looked at nothing); window resize while fog is on (grid is reallocated on a grid change, not exercised);
  profiler capture with the fog on in the editor viewport (offscreen path) is the same code but not looked at; HE_HAVE_SHADERC=OFF builds (the capability is set inside the
  existing `#if defined(HE_HAVE_SHADERC)` block of `GetCapabilities` and `EnsureFogPipelines` returns false without it; not built that way).
- **Decisions taken:**
  - `heApplyFogT` and `heFogVolume` live behind `#define HE_FOG_VOLUME`, defined only by `injectPreamble(..., Backend::Metal)` for material fragments.
    Reason: the preamble is shared; defining the binding for every backend would change GL/Vulkan/D3D12 translucent materials (root signature, GL units) in mode 0
    and the step's limits forbid touching them. Steps 4-6 flip the define per backend when they bind 34 (and add the HLSL t34/GL unit pins).
  - Sky pixels get only the near volume (Tv, Lv at D), no analytic far field (plan said clamp to 1e5 m and rely on identity): the sky is not fogged in mode 0
    and the identity only holds if the sky equals `heSkyEnv`, which it does not (sun disk, clouds).
  - `fogViewProj` carries the y flip so one `uv = clip.xy/clip.w*0.5+0.5` serves every backend later (froxel y = 0 at the top, Metal's fragment origin).
  - The shadow lookups got `HE_SHADOW_PCF_R` (default 1 = the surfaces' 3x3, identical maths); the fog kernel defines 0 (one tap, the slice jitter and the
    temporal blend filter). Plan E8 said "1 tap".
  - The fog has its OWN 64^3 noise volume (`HE::BuildSkyNoise3D(64)`, 1 MB, same generator, created with the pipelines) instead of the sky's 256^3 one, and one
    tile spans 16 x `fogNoiseScale`: sampling the 67 MB volume once per froxel with a 40 m tile cost 5.5 of the 8.2 ms of Inject (bisected with kernel switches).
    The sky noise bytes are untouched. Deviation from plan §5.1 ("no new noise texture").
  - Gains: `sunGain 0.3`, `lightGain 0.1` (HG normalised to 1 for the isotropic phase), chosen by eye; env `HE_FOG_SUNGAIN`, `HE_FOG_LIGHTGAIN`.
  - `fogViewProj`/Apply constants use the unjittered camera; the history is reprojected with `prevViewProj * pos`, `pc.w` as previous planar depth.
  - Removed the step-2 feasibility probe test (it text-pinned the old PCF loops; the production kernels now prove the same path).
  - Debug switches (all env, all read in `MakeFogVolumeData`): `HE_FOG_MODE` (overrides the scene's fogMode), `HE_FOG_PARITY`, `HE_FOG_DENSITY`, `HE_FOG_SUNGAIN`,
    `HE_FOG_LIGHTGAIN`; `HE_FOG_TIMING=1` (Metal: Inject/Integrate on their own waited command buffers, logs medians; the frame is wrong while it is on).
- **Decisions suggested, not taken:** a Quality tier that skips the sun/cloud/local-light terms entirely for Low; async compute for Inject; a cheaper
  Apply (linear depth in a memoryless attachment, plan §8); shrinking the grid with the render scale.
- **Pending (step 3 left open):** the pass cost is NOT at the §8 target on this machine (below); no scene with real graph-material terrain was measured;
  `MTL_DEBUG_LAYER` not run; the sun shaft look is soft.
- **Traps:**
  - `EncodeScene` ends its encoder after the sky when the fog runs and hands the reopened one back through `m_fogSceneEncoder` (caller: `sceneEncoder =
    CFBridgingRelease(...)`); anything that must touch the encoder after `drawSky()` in `EncodeScene` has to use the local `encoder`/`renderEncoder`
    (both are reassigned). The `Scene` GPU timer pair spans both encoders (`ftBeginMulti` + `ftAttachStart/End`).
  - Detailed capture (`kDetailedPassCount = 9`): Inject/Integrate land in the `Shadow` command buffer and Apply in `Scene`; do not add `flushPass` names
    (the accumulator counts 9). Non-detailed stage counters overlap (FogInject reads 8 ms there: not an exclusive cost).
  - `HE_FOG_TIMING`'s isolated command buffer reads ~2.4 ms even for a kernel that only stores a constant (GPU wake/overlap with the previous frame):
    use the detailed-capture delta for cost, the isolated number only for A/B of kernel variants.
  - Heredocs/long shell: none. `he_shot.py` forces SKYTEST; the dump scenes need explicit `HE_DUMP_FOGDENSITY` because a loaded scene carries serialized values.
  - Fog falloff is relative to world y = 0: a camera at y = 312 (underwater witness scene) sees almost no height fog at the same density.
  - The dump is not bit-stable (1 LSB noise); compare with a control run of the same binary first.
- **Next steps:**
  1. Step 4 (OpenGL fragment path): add the GL texture unit for 34, define `HE_FOG_VOLUME` in `injectPreamble` for the GL backends, fill `fogVol` in `fillMatLight`,
     reuse `FogVolumeShaderSource.h` (the Apply/Integrate sources are fragment-friendly; Inject needs a per-layer fragment wrapper).
  2. Steps 5-6 the same for Vulkan / D3D (HLSL pin t34 with a dead sampler, `D3D12MaterialRootSignature.h` null SRV t34, `D3D11MaterialBindings.h`).
  3. Cost: see "Measured cost"; the next lever is Inject (see below). Try a no-shaderc Metal build once (nothing else in this step exercised it).

## Measured cost (Apple M5, MacBook Air, battery 23-30 % discharging, Low Power Mode off, display awake)

Setup: `scripts/he_perf_capture.py --detailed`, Catania `StartupScene` (project cloned to /tmp, user project untouched), render size 2840x1528, grid Medium
(160x81x64), `HE_FOG_MODE=0|1 HE_FOG_DENSITY=0.02 HE_SKY_TIME=10`, 300 frames after 200 warm-up, 4 runs per mode, run order alternating. p50 per pass, median over the 4 runs:

| | fog off | fog on | delta | §8 target |
|---|---|---|---|---|
| `Shadow` bucket (= shadow maps + **Inject + Integrate**) | 0.91 ms | 1.47 ms | **+0.56 ms** | Inject 0.30 + Integrate 0.05 |
| `Scene` bucket (= scene + **Apply** + encoder split) | 13.10 ms | 13.26 ms | +0.16 ms (inside the run spread of about 0.3) | Apply 0.15 |
| whole frame GPU (median) | 16.83 ms | 17.36 ms | **+0.53 ms** | <= 0.5 ms total |

Per run: Shadow off 1.21 / 0.80 / 1.02 / 0.79, on 1.71 / 1.69 / 1.25 / 1.26 (the machine is thermally/battery limited, runs swing by 0.4 ms).
`HE_FOG_TIMING` isolates Integrate at 0.06 ms (target 0.05). The per-pass split of Inject vs Apply is therefore not exclusive; the sum is about at the 0.5 ms
total, Inject is the largest part. Before the noise change Inject alone cost +2.5..4.5 ms in the same detailed capture (bisection with kernel switches in
isolated buffers: noise 8.2 -> 2.7 ms of the isolated number, everything else switched off still 2.4 ms = measurement floor).
Left on the table for Inject: the 9-tap local-shadow and the CSM tap are already 1-tap; remaining cost is the 3 dependent texture fetches (history, noise x2,
sky cube) and ALU; a Low tier (96x54x32) quarters the work.
