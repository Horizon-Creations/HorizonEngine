# Export optimisation, Windows pass (Thema 228, step 1589) — 2026-11-02

Machine NN-WS03 (RTX 4070, Windows 11), MSVC VS18, Release, Ninja, tree `C:/h228`, source `C:/hr/228` (worktree of release/0.8.0).

## Handoff

### Done and verified
* **Build**: `he_tests`, `HorizonGame`, `HorizonEditor` build clean (Release). First build failed to link `test_gltf_material_import` /
  `test_simple_texture_material`: `HE::TextureRef` and `HE::SimpleTextureMaterial` lacked `HE_API`
  (`src/HE_Core/include/Hpak/SimpleTextureMaterial.h:27,56`). Fixed, commit 8ab0c41a.
* **Tests**: full `he_tests` 6125/6125 cases, 2158714/2158714 assertions, 20 skipped, 0 failed (scratch APPDATA,
  `HE_NET_LOOPBACK_ONLY=1`). `test_hpak` "Disk registry" and `test_texture_bcn` pass on Windows (the 9e490d1c fix holds).
  `scripts/editor_help_audit.py --check`: 1252/1252.
* **B1 CPU pixel release on D3D11/D3D12/Vulkan** (8ab0c41a): `D3D11Renderer.cpp` `createAlbedoSRVReleasing`;
  `D3D12Renderer.cpp` `uploadTexture2DReleasing` (+ `allocAlbedoSlot(..., cm)`); `VulkanRenderer.cpp` `uploadTextureImage`
  restores at the top and releases at the end. Safe because every path copies the bytes before it returns (immutable
  `CreateTexture2D`; D3D12 memcpy into the upload buffer; Vulkan `stageUpload` memcpy into the staging ring).
  Measured with a generated project (20 noise textures 2048^2, BC7 pak 114 MB), 1500 frames, median working set from
  t=6 s, two runs each: D3D11 318 -> 421 MB (off/on of `HE_KEEP_CPU_PIXELS`), D3D12 356 -> 462, Vulkan 572 -> 678;
  i.e. ~105 MB saved on each, 112 MB expected. Image at frame 250: identical below the animated sky band (top quarter)
  for keep 0 vs keep 1 on all three; noise floor (keep 0 vs keep 0) is the same 0 px there. 0 `ERROR` log lines.
* **B3**: `RenderPath` default 1 starts "deferred path ready" and "deferred frame" on D3D11, D3D12 AND Vulkan (all three
  report `supportsDeferredRendering`; the task text said only Metal/GL). Forward vs deferred differ in ~14 % of the
  pixels (max delta 122-150): deferred cubes are brighter/hazier. Not analysed, pre-existing, not caused by this work.
* **B5**: `Audio engine ready` is logged from a non-main thread on WASAPI, sound device opens fine;
  `SkyNoise3D: baked 256^3 in 44 ms`. D3D11 warm start: window+splash 293-318 ms, renderer 294-304 ms,
  **OnInit 70 ms (Mac: 4 ms — something else in OnInit costs ~65 ms on Windows, not looked at)**,
  first frame presented 896-911 ms since Run (cold D3D shader compile: 3033 ms).
* **B6**: `phonon.dll` (53 MB) is left out by default and shipped when the audio buses choose Steam Audio; that game
  starts and logs `Steam Audio 4.8.1 ready`. Bench switch `HE_REACH_STEAMAUDIO=1`.
* **B2 (partly)**: pak with baked PSHD variants (`HE_REACH_SHADERS=14` = Vulkan+D3D11+D3D12, +1.5 MB) starts and renders
  on all three, no errors. NOT proven that a baked graph shader is actually read: the scene draws no graph material
  (first-frame compile counters are equal with and without the bake).

### Not verified
* B4 mesh texture bake in a real game picture (only the unit tests: `test_mesh_bake_pass`, `test_mesh_texture_bake`).
* B7 editor UI (builds, help audit green; never opened — the `horizon-editor` MCP server did not connect).
* B2 with a graph material; B8 not touched (needs the user's okay); Teil D not started (waits for Thema 326).
* Vulkan lavapipe job; foliage test project (absent on this machine, see below).

### Tools added
* `tests/test_bench_export_reach.cpp`: `HE_GEN_PROJECT=<dir>` (+`HE_GEN_TEXTURES`, `HE_GEN_SIZE`) writes the texture-heavy
  project used above; `HE_REACH_STEAMAUDIO`. Method for numbers: export with `HE_REACH_PROJECT/RUNTIME/OUT/ENGINE/CACHE`,
  `HE_REACH_TEXCOMP=2` (BC7), write `data/config.json` `{CustomConfig:[{Key,Value}]}` (`GameBackend`, `GameWindowMode=Windowed`,
  `RenderPath`), run the exe with `HE_HIDDEN_WINDOW=1 HE_EXIT_AFTER_FRAMES=N`, poll `Process.WorkingSet64`.

### Next steps
1. A graph-material scene to prove B2 (baked shaders read, `compiles=` drops).
2. Find the 65 ms of OnInit on Windows.
3. Look at the deferred-vs-forward brightness difference on D3D/Vulkan.
4. B4/B7 by hand in the editor.
