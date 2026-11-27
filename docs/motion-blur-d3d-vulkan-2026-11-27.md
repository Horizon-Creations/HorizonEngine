# Motion blur on D3D11, D3D12 and Vulkan (27.11.2026, NN-WS03)

The camera motion blur of GL / Metal (Thema 49, `6578258a0`) ported to the three Windows backends. Branch
`claude/motion-blur-d3d-vulkan` (off `release/0.8.0`).

## Handoff

**Done**

- Shaders: `HlslSources.h` `kMbVelocityHLSL` / `kMbBlurHLSL` (D3D11 + D3D12), `shaders/motionblur_velocity.frag` /
  `motionblur_blur.frag` (Vulkan, in the `HE_VULKAN_SPV` list). GL's two passes line for line; the depth convention per
  backend: D3D = GL ndc z with uv origin top-left (Metal's case), Vulkan = clip-fixed z, ndc y down like uv.
- `HorizonRendering/MotionBlurShared.h`: `motionBlurReproject` (prevViewProj * inverse(viewProj) in **double**) and
  `motionBlurParams` (max blur at 720p scaled to the height).
- Each backend: after the underwater fog, before the bloom; velocity (RG16F px) → smear into its own target → copied
  back over the HDR target, so bloom and tonemap read the blurred image unchanged. Runs only in real frames
  (`mbRealFrame`, set around `DrawViewportFrame` in `Render()`); the previous camera moves with every real frame, on or
  off. Not for a raw capture, an orthographic camera, RVT pages or `RenderSceneImage` stills.
  - D3D11 `D3D11Renderer.cpp` (fields beside the underwater fog, pass in `DrawViewportFrame`, targets lazily).
  - D3D12: root sigs of the fog's shape (24 root constants), `postFxSrvHeap` 14 → 16 (`k_mbSrvBlur`: hdr, velocity),
    `ensureMotionBlurTargets` (waits for the GPU, only on first use / resize), pass in `runPostFX`.
  - Vulkan: `m_mbVelRP` / `m_mbRP`, two sets from `m_postFxDSPool` (pool 13 → 16 samplers, 7 → 9 sets), `makePipe` takes
    an optional layout, targets made with the post chain (12 B/px even when off: `shortcut:` comment there), hdr image
    gets `TRANSFER_DST`.
- Editor: the Preferences row is enabled for every backend but Software; help, catalog and `IRenderer.h` texts updated.

**Verified** (Release, RTX 4070; `cap.ps1` here: HE_DUMP SHADOWINSTTEST, HE_SKY_TIME 100, AA/bloom/DoF off; `ana_band.py`
on the cube band):

| | still: MB on vs off | 3° yaw: h / v gradient | 3° pitch: h / v |
|---|---|---|---|
| OpenGL (reference) | identical | 0.78 / 0.92 | 0.81 / 0.43 |
| D3D11 | identical | 0.69 / 0.91 | 0.66 / 0.30 |
| D3D12 | identical | 0.69 / 0.91 | 0.66 / 0.30 |
| Vulkan | identical | 0.68 / 0.91 | 0.67 / 0.30 |

Streaks of the same length and direction on all four (`mb-backends.jpg`: per row GL, D3D11, D3D12, Vulkan; yaw off/on,
pitch off/on). The D3D/Vulkan scene is lit brighter than GL's (25 levels on the band before any blur), which is why
the gradient ratios differ a little. Vulkan log: 0 warnings, 0 errors. `test_motion_blur.cpp`: the reprojection
of a still camera 38 km out with the 250 km far plane is 0.00002 px in double; **the float inverse GL / Metal use
gives 92.8 px**; both HLSL passes compile on FXC. `editor_help_audit.py --check` 1300/1300.

**Not verified:** a camera flight by hand in the interactive editor; D3D12 under the debug layer; GPU cost of the pass;
the export (the game runtime pushes the same settings, `GameApplication.cpp`).

**Decisions:** copy back over the HDR target instead of re-pointing bloom/tonemap (one copy, the chain untouched);
real frames only (an RVT page or a still would otherwise be the "previous camera" of the next frame).

**Next steps**

1. GL and Metal: use `HE::motionBlurReproject` (one line each, `OpenGLRenderer.cpp` `RenderMotionBlur`,
   `MetalRenderer.mm` ~14018). Today a still camera far from the origin (tile worlds) blurs the whole frame there.
2. Per-object velocity (objects moving past a still camera stay sharp on every backend).
