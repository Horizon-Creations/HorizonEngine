# Editor: texture arrays go to the GPU block-compressed (2026-11-04)

Shared EngineContent keeps the landscape's texture arrays as platform-neutral RGBA8 (13 slices of 2048², 291 MB per
array, 3 arrays per group; `docs/landscape-slope-variants-2026-11-03.md` says why: BC7 is no Apple format). An export
already cooks them for its target (`PackSettings::cookTextureArrays`), the editor did not, so every array sat in RAM and
VRAM at full size. Now the editor cooks them for the GPU it runs on, when they are read.

## What it does

* `ContentManager/GpuArrayCook.{h,cpp}` (HE_Core): `readGpuCookedArray(path, out)`. For a loose `.hasset` of at least
  8 MB whose first bytes say "RGBA8, 4 channels, layers > 1" it returns the file as the GPU wants it, otherwise false and
  the caller reads as before. Cooking is the export's own `Hpak::cookTextureBlob` (every slice and level as it stands,
  slice means for the GI landscape baked into `CHUNK_TXSM`, slices encoded on all cores: BC7 ~2.5 s per 277 MB array).
* Cache: `<userDataDir>/EngineContentCookCache/f<format>q<quality>/<hash(path)>_<size>_<mtime>.hasset`. A hit reads the
  cooked file (73 MB) instead of the source (291 MB); a changed source (size or mtime) is a new key and the old entry of
  that path is deleted. The source file is never written. Written atomically (tmp + rename).
* Hooks in `ContentManager.cpp`: the async worker (before it opens the file) and the synchronous `loadAsset`.
* Format per backend (`gpuArrayCookFormatFor`): D3D11/D3D12/Vulkan/OpenGL → BC7 (quality 1); Metal → ASTC 4x4;
  macOS OpenGL → BC3; Vulkan on Apple (MoltenVK) and the Software renderer → stay RGBA8. Set once in
  `EditorApplication::OnInit` after the backend is known (`Texture arrays for the GPU: BC7` in the log).
  `HE_EDITOR_ARRAY_COMPRESS=none|bc7|astc|bc3` overrides it (there is no settings-panel switch yet).
* The game, `asset_compiler` and every other tool leave the setting off: arrays read exactly as before.

## Verified (Windows, RTX 4070, Release build in `C:/hb/tex`, `-DDEPLOY_DIR` away from the user's deploy)

* `tests/test_gpu_array_cook.cpp` (3 cases, 35 assertions): off by default / small files / missing files untouched;
  cook once, cache hit returns identical bytes with no second cache file, loaded asset is BC3 with 4 slices, srgb and
  12 slice means, the by-path `loadAsset` gets it compressed too, a changed source replaces the cache entry, an already
  compressed source is not cooked again, the source bytes are unchanged. `test_texture_bcn` 16/16,
  `test_contentmanager` 92/92, `test_landscape_groups` green.
* Witness (`scripts/landscape-3-0-accept/cap233_win.ps1`, new editor, RGBA8 arrays from the server upload in a private
  cache, hidden window): Desert and Forest D3D11 frames are **byte-identical (MD5)** to the frames rendered from the
  BC7 files `landscape_tex_gen --compress bc7` wrote, with `277 MB -> 69 MB in 2.3-2.6 s` per array in the log and no
  missing-asset logs.

## Not verified

* **VRAM, measured:** `nvidia-smi` total and the per-process `GPU Process Memory` counter both read 130 MiB for the
  compressed and the `none` run, so the measurement does not see the arrays (hidden window, short run); the saving
  (4x per array, 291 MB -> 73 MB) is the payload sizes in the log, not a VRAM reading.
* Metal/ASTC and macOS GL/BC3 (no Mac): the format choice and the ASTC encoder are there (the encoder cooked the Desert
  arrays in 46 s earlier), the Apple upload of a cooked array is the export's existing path, not run here.
* The first load of a new download stalls for the cook (about 3 x 2.5 s for one group on this 32-thread machine, more
  on a laptop); it runs where the asset is read, the log says it once. Later starts read the cache.
* Anything else that reads an array's pixels on the CPU: only the GI landscape means (baked) and the renderers do;
  the Content Browser thumbnail of a cooked array is not made any more (`AssetThumbnailCache` handles RGBA8 only).

## Handoff

* **Topic / step:** editor-side GPU compression of texture arrays, follow-up of the slope-variants step. Branch
  `release/0.8.0`, commit below (not pushed).
* **Files:** `src/HE_Core/{include,src}/ContentManager/GpuArrayCook.{h,cpp}`, `ContentManager.cpp` (two hooks, search
  `readGpuCookedArray`), `src/HE_Editor/EditorApplication.cpp` (`OnInit`, search `Texture arrays for the GPU`),
  `src/HE_Core/CMakeLists.txt`, `tests/test_gpu_array_cook.cpp` + `tests/CMakeLists.txt`.
* **Decisions taken:** cook on read + disk cache (not at download, so no ContentSync change); quality BC7 1 / ASTC and
  BC3 0; env override instead of a settings switch (a switch needs an `EditorHelp.cpp` entry and config key).
* **Decisions suggested, not taken:** a settings-panel switch and a "clear cook cache" button; eviction of the cook cache
  (it only ever drops the old entry of the same path); pre-cooking in the background after a download so the first
  load does not stall; thumbnails for compressed arrays via `decodeTextureToRGBA8` (BC7/ASTC have no CPU decoder).
* **Traps:** PowerShell's `$home` is read-only: assigning it in a test script silently targets the real home (this
  step created and removed `C:\Users\conno\HorizonEngine`, `cfg`, `f.bmp` that way, nothing else was touched);
  `SDL_GetPrefPath` ignores `%APPDATA%` for other caches, the cook cache uses `GlobalState::userDataDir()` which does follow it.
* **Next steps:** 1. Mac: run `cap233.sh` with a Metal editor, expect `Texture arrays for the GPU: ASTC 4x4`, compare
  against the export's ASTC arrays; 2. a real VRAM reading (RenderDoc / D3D11 debug layer / Instruments); 3. the
  settings switch above.
