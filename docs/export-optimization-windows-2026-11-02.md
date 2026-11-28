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
  **OnInit 70 ms (Mac: 4 ms — but 59 ms of it is `Physics world initialised: 1 rigid body (1 landscape)`, the heightfield of
  THIS scene (HEProject has a landscape; the Mac number was the foliage project); not Windows-specific)**,
  first frame presented 896-911 ms since Run (cold D3D shader compile: 3033 ms).
* **B6**: `phonon.dll` (53 MB) is left out by default and shipped when the audio buses choose Steam Audio; that game
  starts and logs `Steam Audio 4.8.1 ready`. Bench switch `HE_REACH_STEAMAUDIO=1`.
* **B2**: generated project with 20 graph materials (`HE_GEN_GRAPH=1`), deferred default. Baked variants
  (`HE_REACH_SHADERS=14`, pak 2.07 -> 7.01 MB) vs none: D3D11 first frame `compileMs` 7662 -> 21 (121 compiles, 81 -> 121
  cached); D3D12 `frameBusyMs` 254 -> 113, Vulkan 145 -> 62. Image identical below the animated sky band. D3D12/Vulkan have
  no compile counter, the frame-time drop is the evidence there. The pak bakes no G-buffer twins for D3D/Vulkan (as said),
  the deferred frame still runs error-free.
* **B4**: project with one mesh of two quads on two textured materials (`HE_GEN_MESHBAKE_PROJECT`). Export log:
  `Meshes/Two.hasset: baked lossless, 2 sections into one, atlas 256px`. Game frame bake vs `HE_REACH_NOBAKE=1`, quad
  region only: D3D11 2.6 % px differ (max delta 11/255), D3D12 0.8 % (6), Vulkan 1.0 % (7). With
  `HE_GEN_MESHBAKE_OVERRIDE=1` (entity has a material override) the log says `left as it is: an entity draws it with a
  material of its own` and the frame matches (max delta 2-6) on all three. The bench now passes `bakeSkipMeshes` the
  way the export dialog does (`HE::collectMeshesWithMaterialOverrides`).
* **B7 (Mesh Viewer)**: new `tests/test_static_mesh_editor_ui.cpp` drives the real panel headless: each "Export bake"
  checkbox (Vertex colors, Merge textures, One UV texture) flips its switch and a fresh load of the .hasset carries it
  (24 assertions); `HE_UI_DUMP_DIR` gives the frame. Also looked at the real editor (private APPDATA, window shown not
  activated, `PrintWindow`): project + restored "Two.hasset" tab load, no errors. **Export dialog**: only read
  (`ExportDialogPanel.cpp:1212` checkbox, `:564/:580` profile field, `:1443-1516` override scan, `:1726-1727` into
  `ExportSettings`) plus `test_export_profiles`; the dialog itself was not clicked.

### Not verified
* The Export dialog's "Bake textures onto meshes" checkbox by clicking (no UI harness for that dialog; no real mouse
  clicks, they would move the human's cursor).
* Vulkan lavapipe: a Linux CI job, not runnable here; B8 not touched (needs the user's okay); Teil D not started
  (Thema 326 has not answered the HLOD request).
* Deferred vs forward brightness on D3D/Vulkan (see `docs/deferred-vs-forward-brightness-2026-10-02.md`: forward only
  shades the first 8 lights; not re-analysed for this one-sun scene).

### Tools added
* `tests/test_bench_export_reach.cpp`: `HE_GEN_PROJECT=<dir>` (+`HE_GEN_TEXTURES`, `HE_GEN_SIZE`) writes the texture-heavy
  project used above (+`HE_GEN_GRAPH=1` for graph materials; `HE_GEN_MESHBAKE_PROJECT=<dir>` [+`HE_GEN_MESHBAKE_OVERRIDE=1`]
  for the two-material mesh); `HE_REACH_STEAMAUDIO`. Method for numbers: export with `HE_REACH_PROJECT/RUNTIME/OUT/ENGINE/CACHE`,
  `HE_REACH_TEXCOMP=2` (BC7), write `data/config.json` `{CustomConfig:[{Key,Value}]}` (`GameBackend`, `GameWindowMode=Windowed`,
  `RenderPath`), run the exe with `HE_HIDDEN_WINDOW=1 HE_EXIT_AFTER_FRAMES=N`, poll `Process.WorkingSet64`.

### Next steps
1. Click through the Export dialog once (bake checkbox, profile save/load).
2. Look at the deferred-vs-forward brightness difference on D3D/Vulkan in a one-sun scene.
3. Teil D once Thema 326 answers; B8 only with the user's okay.
