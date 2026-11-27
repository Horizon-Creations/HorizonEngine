# Tile world: far image baked in the editor, and drawn under the material (topic 355, step 5, 27.11.)

## Handoff

**Done** (branch `claude/landscape-one-system`):

1. **Editor bake of the far image** (`ae6b126c1`).
   - `src/HE_Editor/LandscapeFarBake.{h,cpp}`, the tile half: `setTileRuntime`, `bakeTileWorld`, `tickTile`.
   - It runs the export's region bake (`HE::bakeLandscapeFar`, weight painter) over the edit overlay, on a `std::thread` with cancel.
   - The result goes to the export's own cache, `Saved/LandscapeFar/<pack>.far` (`ExportDialogPanel` `farCacheDir`), and onto the overlay with `LandscapeEditSource::setFarImage`; the runtime takes it up on `coarseChanged()`.
   - Setting: `EditorConfig::LandscapeFarBake`, the same one the old landscape uses: Off / Manual (Tile Landscape ▸ Far Image ▸ **Bake Far Image**) / Automatic (4 s after `LandscapeRuntime::coarseRevision()` last changed).
   - A reopened world takes the cached image. Automatic re-checks it, which keeps unchanged regions.
   - New engine accessors: `LandscapeRuntime::editsShared`, `coarseRevision`; `LandscapeEditSource::setFarImage`, `baseShared`.
2. **Far image under the material, Metal** (this commit).
   - `kLandscapeMaterialFragmentHead` gets `HE_LS_FAR`: `heLsFar()` reads the far image at binding 62.
   - `landscapeFragmentSource(..., farImage)` puts an early-out at the top of `main` for the lit forward and the G-buffer variants: a fragment whose tile is not in the atlas writes the far image's colour and AO (geometric normal, roughness 0.9) and returns before the graph.
   - MSL pin 62 → texture 31, with a new `MslOptions::constexprMipLinearSamplers` (a mip-linear inline sampler).
   - `MetalLandscape` binds the image on 31 in the material branch and sets `uTile.w` = 1 when there is one.
   - `HE_LS_FAR=0` turns it off (the A/B switch).
   - **Texture 31:** the old comment "25-30, the last free ones" assumed the 31-entry argument table. An M5 builds pipelines with textures 31, 40, 96 and 127; Apple GPUs and the Mac family take 128 per stage.

**Verified**

- `landscape_gpu_check --only metal` and `--only gl` exit 0; the material still draws.
- `ctest`: see the commit.
- Help audit green.
- 100 km Catania tile world, headless, Automatic:
  - 625 regions baked in 7.9 s on the worker, the cache written, no frame-hitch warning;
  - second start: 0 painted, 625 kept, image from the cache.
- A/B at 1000 m (`HE_LS_FAR` 0/1): 7 182 of 1.9 M pixels differ (only the ground beyond the atlas); GPU p50 14.68 vs 14.72 ms (no gain there).

**Not verified**

- An edit followed by an automatic re-bake of only the touched regions (the hashing says so: an unchanged world paints 0).
- Frame times under 33 ms during a bake (only "no hitch warning").
- The editor panel by eye.
- MoltenVK, GL 4.6, D3D11/12: they still evaluate the material over the coarse weights beyond the atlas. On Vulkan, the far image would need a free descriptor.

**Decisions taken**

- The far image is drawn only where the tile is NOT in the atlas. It is 16 texels per tile (≈32 m on 512 m tiles), too coarse to stand in for atlas tiles that only lie outside the RVT window.
- **Step 4's remaining cost** (the full material over atlas tiles beyond the 4 km RVT window, seen from 200 m and up) is therefore not removed by this step. The fix would be a coarse whole-world RVT volume, or a far image at the RVT's coarse mips. This is open for the queen to decide.

**Next steps**

1. Witness an edit → automatic re-bake of only the touched regions.
2. The same early-out on Vulkan/D3D/GL 4.6 (Windows, NN-WS03).
3. The cost beyond the RVT window inside the atlas (see the decisions above).
4. Step 6 (mesh HLOD per cell).
