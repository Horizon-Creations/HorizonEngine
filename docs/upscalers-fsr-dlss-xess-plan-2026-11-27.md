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
  3. **Availability in the UI** (`360d2b3fa`): `AAMethod::DLSS = 6`, `XeSS = 7` (no backend has them;
     fallback DLSS/XeSS → FSR → …); `Row::comboAvailable` greys an entry out with "(unavailable)" and a tooltip
     why; the capability list shows FSR/DLSS/XeSS/render scale/frame generation.
  4. **HorizonCode** (`7d9f51b09`): eleven `settings.*` rows (PlayerSettings.cpp:286-400, EngineApi.cpp:7198):
     `upscalers`, `isUpscalerAvailable`, `set/upscaler`, `upscalerModes` (NativeAA … UltraPerformance, the
     ratios of `ffx_upscale.h`), `set/upscalerMode`, `frameGenerationModes` (ints > 1), `isFrameGenerationAvailable`,
     `set/frameGeneration` (1 = off). Lists from `Host::graphicsSupport` (renderer caps at call time); a setter
     given what the machine cannot run warns and keeps the old value; saved with `settings.save`. The game's
     per-frame AA push lays the choice over the config (`settings::aaMethodFor` / `renderScaleFor`,
     GameApplication.cpp:3708); the editor's Play only records it (like VSync). New caps `supportsRenderScale`,
     `maxFrameGeneration` (IRenderer.h:348). `registry.json` NOT regenerated (its dump script is macOS-only).
  5. **Render scale / upscaling with FSR on D3D12**: `sceneW/H` (D3D12Renderer.cpp:1992, `IRenderer::UpscaleRenderSize`,
     clamp [1/4, 1]) sizes depth, `hdrRT`, TAA/velocity and every DrawScene pass; `fsrOut` (display size, made in
     `createPostFXResources`, slot 1) is what the tail reads (motion blur via `tailHdr()`, bloom, tonemap, AA, UI);
     caustics/fog draw at the scene size; FSR runs in every upscaled frame (unjittered + reset outside `taaFrame`).
     Context sized render → display (recreated on a scale change). Stills (RVT) stay unscaled; `rvtStillLeave`
     restores the scene size. `supportsRenderScale = supportsFSR` on D3D12; the Preferences slider needs FSR there.
     The FSR loader is found beside the module (`GetModuleHandleEx`), not the exe: the game keeps the engine in
     `HorizonEngine/`, where FSR never loaded before.
  6. **Frame generation, FSR 3.1 FG on D3D12** (game swapchain only, needs FSR as the AA method): own CMake files +
     `HE_HAVE_FSR_FG` (`amd_fidelityfx_framegeneration_dx12.dll`, 30 MB, signed by AMD, SHA256-pinned).
     `D3D12Fsr::replaceSwapchain` — AMD sample's pattern, NOT Wrap: the hwnd's swapchain is released and the
     swapchain context makes the proxy `ForHwnd` (first FG frame, once per run, never back); FG context at the back
     buffer size (recreated after `ResizeBuffers`); per frame `ffxConfigure` (frameID +1, `frameGenerationCallback`
     dispatches inside Present, HUD-less = `viewportRT` copied before the UI pass) + `PrepareV2` on the upscaler's
     depth/velocity/jitter; teardown FG off → FG ctx → swapchain ctx → swapchain. `IRenderer::SetFrameGeneration`,
     `maxFrameGeneration = 2` when the provider answers; `settings.frameGenerationModes` is empty unless the
     upscaler is FSR. Game push per frame; `HE_GAME_FRAMEGEN=n` forces it, `HE_FSR_FG_STATS=1` logs generated /
     rendered, `HE_FSR_FG_DEBUG=1` AMD's tear lines.
  7. **Reactive mask** (`714b08504`): particles, glass and water have no velocity. `hdrRT` copied right before the
     blended tail (`fsrOpaque`), FSR's GENERATEREACTIVEMASK right after it at the end of DrawScene (AMD sample's
     scale 1 / cutoff 0.2 / binary 0.9 / tonemap+threshold+max), handed to the upscale dispatch. Fog and the
     underwater passes come after it, so they stay out. Witness switches `HE_DUMP_GLASSCUBE=<opacity>`,
     `HE_FSR_REACTIVE_STATS=1`, `HE_FSR_REACTIVE_PGM=<file>`, `HE_FSR_DEBUGVIEW=1`.
  8. **Skinned velocity** (`d406c6cd0`): the velocity pass redraws what the skinned pass drew
     (`skinVelDraws`, its bones-ring address) with `VSVelocitySkinned` (HlslSources.h, skins exactly like
     VSMainSkinned) and last frame's palette per entity (`taaPrevSkins`, second ring `m_prevBonesCB`). TAA, FSR and
     FG all read it. Witness `HE_DUMP_SKINTEST=<deg/frame>` (two-joint column), `HE_TAA_VELOCITY_STATS=1` /
     `HE_TAA_VELOCITY_PGM`, control `HE_TAA_SKINVEL=0`.
  9. **Mip bias** (AMD: `log2(render/display) - 1`, `IRenderer::UpscaleMipBias`): `frameMipBias` in DrawScene (FSR
     frames only) → `uGIParams.w` (scene shader's albedo `SampleBias`, PSMain + GBufPS) and `lit.giParams.w` =
     `HE_TEX_BIAS` (kLightingPreamble) for graph materials: `MaterialGraph::texBias` adds it to fragment-stage
     `texture()` reads (not `vs`/`ds`, not UI), `texGradScale` scales the bombed reads' gradients by `exp2`. The
     generated header defaults `HE_TEX_BIAS` to 0, so every other backend is unchanged (giParams.w = 0 there).
     The 5 shipped auto-landscape materials regenerated (+128 bytes each; Mix via a temp dir, its arrays untouched)
     and `DisplacementDemo.hasset` (`mat_gen` into a temp dir; its text differs by the bias only). `Water.hasset`
     NOT taken: its bake also lacks two older generator changes (`vWorldPos + heLight.worldOrigin`, `heApplyFogT`)
     nobody regenerated — a pre-existing stale bake, worth its own `mat_gen` commit.
     **Pre-existing fix on the way:** D3D12's static albedo sampler `s2` had `MaxLOD = 0` (zero-initialised desc),
     so simple textured materials never left mip 0 — they shimmered at a distance. Now `D3D12_FLOAT32_MAX`.
     Witness `HE_DUMP_MIPTEST=1` (1024² one-texel checker floor with its mip chain), control `HE_FSR_MIPBIAS=0`.
- **Verified** (NN-WS03, RTX 4070, Release; `HE_DUMP` SHADOWINSTTEST, 40 settle frames, cube band): the loader
  picks **FSR 3.1.6**, `HE_FSR_DEBUG=1` reports nothing. Sign A/B against the AA-off frame of the same pose
  (run-to-run noise 0.040): still FSR 0.754 vs jitter-y flipped 0.855; pan (`HE_DUMP_PANYAW=0.5`) FSR 1.239 vs
  motion vectors flipped 1.841 (visible ghost). FSR keeps the edge gradient (2.41 vs 2.39 off). Loader DLL
  renamed: one log line, TAA runs instead.
  Render scale 0.5 (log `FSR 3.1.6 active (640x360 -> 1280x720)`): whole frame (quadrant means = 1.0's), still
  1.35 vs off (1.0: 0.75; softer, no mip bias). Pan at 0.5: 1.35 vs motion-vector scale at display size 1.98 and
  sign flipped 2.18 (FSR run-to-run noise in a pan 0.045; an earlier "0.3-0.7" was a run that inherited a test
  switch, see the trap below). TAA at 0.5 = TAA at 1.0 (0.004: D3D12 does not
  scale without FSR). `HE_DUMP_GPUTIME=300,2560x1440`, 3 interleaved pairs: GPU median 9.96/8.89/8.82 ms at 1.0
  vs 3.74/3.90/4.44 ms at 0.5 (paired −6.2/−5.0/−4.4 ms); that run also resized the upscaled viewport (1280x720 →
  2560x1440 → back, `createViewportRT`). Motion blur + bloom at 0.5 with `HE_GPU_DEBUG=1` (+ `HE_FSR_DEBUG=1`):
  no debug-layer error (only the ClearRenderTargetView clear-value hint), MB acts (yaw-step horizontal gradient
  ×0.68). he_tests: full suite 6618/6621 (the 3 = gamepad tests, a real pad is attached), help audit.
  **Game** (export of a witness project via `ProjectExporter` from a throwaway test, D3D12, AA 5, scale 0.5, 1600x900):
  log `FSR 3.1.6 active (800x450 -> 1600x900)`, `frame interpolation swapchain in place`; `HE_GAME_FRAMEGEN=2`:
  `2640 generated / 2640 rendered = 1.00` (and on); FG off: `0 generated / 13920 rendered`. FG + `HE_GPU_DEBUG=1`
  + `HE_FSR_DEBUG=1`: no error. PrintWindow FG vs no FG: 0.097 mean diff (clouds only). Earlier run counted DXGI
  PresentCount ≈ 2.0 too (unreliable in a window: stalls; hence the callback counter).
  Reactive mask at 0.5: a 0.9 glass cube marks 1.92 % of the frame, exactly the cube (PGM); with volumetric fog the
  fog stays unmarked; a 0.4 glass over white stays under AMD's threshold (1 px), as designed; no tail = no mask;
  debug layer clean. Skinned velocity, 3°/frame, static camera: FSR 0.5 ~700 px moving (mean 1.16 px) only on the
  bending half (PGM), TAA 1.0 ~2900 px (mean 2.26 px); `HE_TAA_SKINVEL=0` 0 px; debug layer clean. Mip bias,
  checker floor far band (luma std): 0.5 without bias 0.81 → with 3.94 (repeat 3.93), 1.0 7.42 → 8.65; near/mid
  unchanged (mip 0 there); a forced +4 blurs it to 0 (the chain works); `test_material_graph` "Texture reads carry
  the upscaler's mip bias" (HLSL `SampleBias(..., heLight_giParams.w)`, `exp2` on the bombed gradients), the
  pinned-text test now proves the bias is the only change.
  **Witness trap:** `cap.ps1` clears `HE_DUMP_*` only — an `HE_FSR_*`/`HE_TAA_*` switch set for one capture stays
  set for every later capture IN THE SAME PowerShell call. Set them explicitly (=0/=1) per capture.
- **Not verified:** FSR 4 (needs an RX 9000); Vulkan; GPU particles (D3D12 draws none: CPU particles are tail
  draws, covered by the mask); the mip bias in motion (more shimmer is AMD's trade-off) and for the RVT landscape
  main view (`heRvtSample` picks its page mip from derivatives — not biased, so RVT-textured ground stays softer
  at 0.5); D3D11's albedo sampler (same zero `MaxLOD`? not looked at); a player choice through
  `settings.setUpscaler` / `setFrameGeneration` in a running game (rows unit-tested;
  the game ran with config keys / `HE_GAME_FRAMEGEN`); FG with a camera in motion judged by eye (counted only);
  FG in fullscreen and across a window resize (code path: `destroyFrameGen` before `ResizeBuffers`);
  `he_perf_capture` in the real loop (the hidden editor drew no viewport here). RVT page ticks: the still bracket
  already rebuilt the post set in Enter and (page size ≠ viewport) in Leave; while upscaled Leave also resets FSR's
  history, like TAA's before — a landscape streaming pages at 0.5 was not looked at.
- **Decisions:** the dispatch sits before motion blur, unlike MetalFX; FSR's own sharpening takes AA Sharpness.
  Render scale on D3D12 only through FSR (no bilinear fallback); no supersampling (> 1 counts as 1).
  Particles get FSR's reactive mask, not velocity (AMD's guidance for alpha-blended effects).
  Frame generation 2× only: FSR 3.1's swapchain path generates one frame; 3×/4× (DLSS MFG, RTX 50) stay
  unavailable. FG renders at most ~half the frames it would without (pacing), as AMD's.
- **Next:** Vulkan FSR + FG (`*_vk.dll`, and the same skinned velocity / reactive mask / mip bias there); the
  RVT page lookup's mip bias; D3D11's albedo sampler MaxLOD; a settings menu in the template wired to the new rows;
  DLSS / XeSS (steps 4, 5).
