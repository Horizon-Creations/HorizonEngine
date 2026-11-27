# Upscalers: FSR, DLSS, XeSS (plan, 27.11.2026)

Owner's request (27.11.): integrate FSR, then DLSS and XeSS natively (super resolution first; frame generation
later). Branch `claude/upscalers-fsr`, on top of `claude/motion-blur-d3d-vulkan` (both rebuild the post chain).

## What the engine has (survey, file:line on that branch)

- `AntiAliasingSettings::renderScale` (IRenderer.h:483) is read by **Metal only** (MetalRenderer.mm:13790,
  sizes :19839-19844). Metal's order is the template: scene, velocity, DoF, fog, motion blur, bloom at render
  size → MetalFX on the HDR image before tonemap (:20328) → tonemap, TAA, AA, UI at output size.
  MetalFX gets color (HDR), depth, velocity (`motionVectorScale = -w,-h`: velocity is uvNow-uvPrev), jitter
  `-m_taaJitter`, reset on recreate; no exposure.
- D3D12 / Vulkan: everything at viewport size; TAA resolves on the tonemapped RGBA8 image (D3D12 :2875,
  Vulkan :1290); jitter = `HE::taaJitter` Halton(2,3) 8 phases in pixels (TemporalAA.h:20).
- Velocity RG16F uvNow-uvPrev (D3D12 HlslSources.h:751, Vulkan taa_velocity.frag:17), written for the tile
  ground, non-skinned objects, foliage; **not** for skinned meshes, particles, the transparent tail, sky (0).
- Exposure: a fixed 1.0 (no auto exposure). No texture mip bias tied to render scale anywhere.
- Third-party binaries: precedent = Steam Audio, `file(DOWNLOAD … EXPECTED_HASH SHA256)` at configure and a
  POST_BUILD copy next to the editor (CMakeLists.txt:742-800, src/HE_Scene/CMakeLists.txt:208-219).

## The SDKs (checked 27.11.)

| | APIs (SR) | GPUs | How it ships | License |
|---|---|---|---|---|
| AMD FSR SDK v2.4.0 (28.10.2026; FSR 3.1.x + FSR 4 behind one API) | D3D12, Vulkan 1.2 (parity since 2.4) | 3.1: any SM 6.2; 4: Radeon RX 9000+ | signed DLLs in the repo `Kits/FidelityFX/signedbin`: `amd_fidelityfx_loader_{dx12,vk}.dll` (39 KB) + `amd_fidelityfx_upscaler_dx12.dll` (57.7 MB) / `_vk.dll` (36.1 MB); headers `Kits/FidelityFX/api/include` + `upscalers/include/ffx_upscale.h` | binary redistribution free, notices required, no reverse engineering |
| NVIDIA DLSS SR 4.5 via Streamline | D3D12, Vulkan (D3D11 via NGX) | RTX | Streamline (open source) + DLSS DLLs | NVIDIA SDK terms |
| Intel XeSS 2.x SR | D3D12, Vulkan, D3D11 (`libxess_dx11.dll`) | any SM 6.4 (DP4a), best on Arc | prebuilt DLLs, github.com/intel/xess | Intel license, binary only |

## Steps

1. **Upscaler slot + render scale on D3D12 and Vulkan** (no SDK yet). Scene, velocity, fog, motion blur at
   render size; an upscale step on the HDR image before bloom/tonemap (Metal's order); bloom, tonemap, AA, UI at
   output size. Without an upscaler the AA resolve stretches bilinearly (Metal's fallback). `AAMethod` gets
   `FSR = 5` (later DLSS/XeSS), `ResolveAAMethod` falls back FSR → TAA, `Capabilities::supportsFSR`.
   Texture mip bias `log2(renderScale)` on material samplers while upscaling. Witness: scale 1.0 byte-identical
   to today; 0.5 renders a quarter of the pixels.
2. **FSR on D3D12 and Vulkan** through the loader DLLs (`ffxCreateContext` / `ffxDispatch`, upscale context per
   size and quality): HDR color, depth (GL ndc z 0..1, not inverted), velocity with scale `(-w, -h)` like
   MetalFX, FSR's own jitter sequence (`ffxQuery` phase count / offset) instead of Halton-8 while it is on,
   pre-exposure 1 (or FSR's auto exposure), reset on resize, cut, quality change. DLLs fetched at configure with
   SHA256 (Steam Audio's pattern), copied next to the editor and into the exported game; the license notice
   into the export. Witness: FSR at 0.5 vs native vs TAA, still and in flight.
3. Velocity for skinned meshes and particles (or FSR's reactive mask for the transparent tail): without them
   they smear under any upscaler.
4. DLSS SR via Streamline (D3D12, Vulkan), same slot.
5. XeSS SR (D3D12, Vulkan, D3D11), same slot.
6. Editor: AA mode list + quality preset (Native AA / Quality 0.67 / Balanced 0.58 / Performance 0.5) in
   Preferences and the export; help entries.
7. Frame generation (FSR FG, DLSS FG, XeSS FG): game runtime only (the editor viewport is no swapchain),
   D3D12/Vulkan, swapchain ownership + HUD-less image; its own topic.

## Handoff

- **Topic / step:** none (owner's direct request, not a Hive topic).
- **Done** (branch `claude/upscalers-fsr`):
  1. This plan; the SDK fetched at configure (`cmake/HeFidelityFX.cmake`, `7785dc13b`; owner allowed the download).
  2. FSR on **D3D12 as native AA** (render size = display size): `AAMethod::FSR = 5`, `Capabilities::supportsFSR`,
     `ResolveAAMethod` FSR → MetalFX/TAA; `D3D12Fsr.{h,cpp}` (loader by absolute path beside the exe, context with
     HDR + auto exposure, `HE_FSR_DEBUG=1` = the runtime's checks + its messages in the log, version logged);
     TAA's targets, jitter and velocity serve FSR (`taaWanted`), its resolve not (`aaTaa` = TAA only); FSR's
     jitter sequence (engine clip shift `(x, -y)`), dispatch after the fog and before motion blur (AMD's order —
     Metal runs MB before MetalFX), output copied back over `hdrRT`; real frames only (`mbRealFrame`); depth
     near plane = `2fn/(f+n)` (the depth is GL ndc z's kept half). Editor: "FSR" in the AA list, hint, help.
- **Verified** (NN-WS03, RTX 4070, Release; `HE_DUMP` SHADOWINSTTEST, 40 settle frames, cube band): the loader
  picks **FSR 3.1.6**, `HE_FSR_DEBUG=1` reports nothing. Sign A/B against the AA-off frame of the same pose
  (run-to-run noise 0.040): still FSR 0.754 vs jitter-y flipped 0.855; pan (`HE_DUMP_PANYAW=0.5`) FSR 1.239 vs
  motion vectors flipped 1.841 (visible ghost). FSR keeps the edge gradient (2.41 vs 2.39 off). Loader DLL
  renamed: one log line, TAA runs instead.
- **Not verified:** FSR 4 (needs an RX 9000); render scale < 1 (step 1 is not done: D3D12 still renders at
  viewport size); Vulkan; skinned meshes / particles under FSR (no velocity: they will ghost); the exported game.
- **Decisions:** the dispatch sits before motion blur, unlike MetalFX; FSR's own sharpening takes AA Sharpness.
- **Next:** render scale on D3D12 (scene/depth/velocity at render size, FSR output and everything after at display
  size), then Vulkan (`amd_fidelityfx_loader_vk.dll`), then velocity for skinned meshes and particles.
