# Far-landscape bake: raw output on D3D11, D3D12, Vulkan, OpenGL (Thema 228) — 2026-11-02

Machine NN-WS03 (RTX 4070), MSVC VS18 Release, tree `C:/h228`, source `C:/hr/228`, on top of bf009b21.

## Handoff

### Done and verified
* `EditorCameraOverride::rawOutput` / `rawSrgb` are honoured by `RenderSceneImage` on D3D11, D3D12, Vulkan and OpenGL.
  Shape is Metal's: the tonemap pass takes a sign on its exposure parameter (`< 0` raw, `< -1.5` sRGB), writes
  `clamp(hdr, 0, 1)` (+ exact sRGB curve); bloom, SSAO, SSR, GI (+ DoF, motion blur on GL), AA (method Off) and the
  render path (Forward) are switched off for the frame and restored after it.
  * shared HLSL `kTonemapHLSL` (`D3D_Shared/HlslSources.h`) for D3D11 + D3D12; `shaders/postfx_tonemap.frag` (Vulkan);
    `kTonemapFS` (`OpenGLRenderer.cpp`). Members `rawCapture`/`rawSrgb` per backend, set/restored in each `RenderSceneImage`.
* `IRenderer::Capabilities::supportsRawCapture` (true on all five backends, D3D/Vulkan: `postFxReady`);
  `ExportDialogPanel.cpp` (`bakeFarLandscapes`) asks it instead of `backend != Metal`; help text updated.
* Oracles, all four backends (HE_DUMP_RHI=<b>): `HE_DUMP_RAWTEST` ordinary 147 193 213 (GL 148), raw 51 102 153 min = max,
  sRGB 124 170 203. `HE_DUMP_HLODBAKE=128 HE_DUMP_HLODMOSAIC=1`: mosaic vs whole 0.319 / 0.000 / 0.106 (Metal 0.32 / 0 / 0.11).
  `HE_DUMP_HLODFAR=128`: "12 far levels with the baked material", real vs baked mean 2.3-2.8 /255 (max 84-91), picture
  checked by eye (D3D12): same orientation and colour, baked softer.
* Real run per backend: bake in the editor on that backend (`HE_DUMP_HLODEXPORT`), pack with the bench
  (`HE_REACH_EXTRA`, `HE_REACH_ONLYREACHED=1`, `HE_REACH_TEXCOMP=2`, `HE_REACH_SHADERS=14`), run the exported game on the same
  backend: log "rebuilt 4 of 4 chunks (4 with a baked far material)", 0 errors; plain export says 0. Frame baked vs plain
  (camera straight down, 427 m): max delta D3D11 27, D3D12 32, Vulkan 12, OpenGL 6; images look the same.
  So the baked material (three samples, `FarMap`) runs on every backend, orientation and sRGB/linear reads are right,
  and all chunks sharing one shader works in a game.
* `he_tests` full: 6138/6138 (incl. test_hlod_baker, test_hlod_assets, test_culling, test_reach_filter).
* Synchronous builders need no wait: the bake took 0.3-0.6 s per 4 chunks (GL begin 1.5 s = its first compile).

### Tool added
* `HE_DUMP_AUTOLANDSAVE=<file.hescene>` (`EditorApplication.cpp`, before the HLODEXPORT block): with `HE_DUMP_AUTOLAND=1` saves
  the witness world, its material (`Materials/AutoLandscapeWitness.hasset`) and a main camera at HE_DUMP_CAMX/Y/Z/PITCH/YAW
  into the open scratch project: a project with a real Auto Landscape for the export bench.

### Not verified
* Export dialog click-through (button "Far landscape"); only the capability check is code. Software renderer: supportsRawCapture false.
* Catania-sized landscapes (256 chunks) and the Mac numbers on Windows; only 4 chunks here.
* Vulkan lavapipe; deferred render path on D3D/Vulkan stays unused for the bake (forward only).

### Next steps
1. Click the export dialog once with a project that has an Auto Landscape.
2. Run the Catania copy through `HE_DUMP_HLODEXPORT` on D3D12/Vulkan for time and pak size.
