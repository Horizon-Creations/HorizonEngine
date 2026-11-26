# Tile landscape: the baked far image (Thema 326, 2026-11-21)

## What the owner asked for (confirmed in the session)

A world-partition-like world: tiles and cells loaded and unloaded one by one, whole regions dropped when far, and at export
an HLOD for the landscape chunks that changed, drawn in the distance from a baked texture, with RVT where it is switched on,
shown in the export's cook step. This step builds the baked far texture for the tile world end to end. It does **not** bring
the landscape material or RVT to the tile world (see "The finding").

## The finding that shaped it

`kLandscapeFS` is **not** the Auto Landscape material. It is a fixed blend: twelve weights, layers 0-3 from the texture
group's albedo / normal / mask arrays in world coordinates, the rest flat colours, its own sun + ambient. So there is no
material graph to photograph for a tile, and baking the real Auto Landscape now would put a visible ring at the atlas edge
(near tiles would not run it). The far image therefore bakes exactly what the tile shader draws; the painter is the one
swappable piece, and the real material takes its place when the tile world draws with it (Thema 335 step 10, waits on PR 233).

## Built

| Part | Where | What |
|---|---|---|
| Table | `LandscapeFarMap.h/.cpp` | One RGBA8 image a **region** (8 x 8 tiles): rgb = albedo (sRGB), a = AO. `encode/decode` (regions as weight-grid blobs), `assemble` (whole world + mips, mean in linear light) |
| Bake | `LandscapeFarBake.h/.cpp` | `planLandscapeFar` (texels a tile from a byte budget and a texture limit, never below the coarsest weight mip), `bakeLandscapeFar` (hash a region = its tiles' stored bytes + painter + layout; unchanged regions are kept from the previous table), `landscapeFarWeightPainter` |
| Pack | `LandscapeSource.h/.cpp` | Second tail section `'HLFM'` **in front of** the cell HLOD section (its trailer stays the last 20 bytes). `LandscapePackWriter::setFarTable`, `LandscapeTileSource::farImage()`, `LandscapePackSource::storedHash/coreSize`, `landscapePackSetTails` (both sections, one copy of the file). `landscapePackSetHlod` is now a wrapper that keeps the far image |
| Export | `LandscapeExport.h/.cpp`, `ExportDialogPanel.cpp` | On the existing "Bake far landscape" switch and budget. Cache: `<project>/Saved/LandscapeFar/<pack>.far`. `landscapeFarPaintFor` = flat layer colours, layers 0-3 from the group arrays' slice means when they load. Lines go to the **cook log** (they went to `Build::log` before, which `Build::begin` clears, so the HLOD line was never seen either) |
| Runtime | `LandscapeRenderFrame.h` (`farW/farH/farMips`), `LandscapeRuntime.cpp` | Assembled when the landscape begins. The generation is now one counter for the process (two runtimes both said 1; a renderer then kept the first one's images) |
| Shader | `LandscapeShaderSource.h` | `uFarAlbedo`, switch `uTexMaps.z`. A pixel whose slot is 65535 reads the far image and skips weights and arrays. All derivatives are taken before the branch; the array accesses became `textureGrad` / `gradient2d` |
| Backends | `OpenGLLandscape.cpp` (unit 33), `MetalLandscape.mm` (fragment texture 30) | upload + bind. `VulkanLandscape.cpp`: binding 13 gets a stand-in, `uTexMaps.z` stays 0. D3D11/D3D12: untouched |
| Check | `landscape_gpu_check` scene 4 and engine pass "far image" | see Verified |

## Verified (Mac, Apple M5, Release, 2026-11-21)

* `he_tests` whole suite **6450 / 6450** on the far-image commit (6441 / 6441 after the merge alone). `test_landscape_far.cpp`: 9 cases.
* `landscape_gpu_check` (all Mac backends, exit 0), scene 4 on a 12 km fBm world with a 16 MB atlas:
  OpenGL and Metal: 52 860 of 69 427 ground pixels come from the far image (marker control: magenta there, **0** atlas pixels moved),
  and the real bake against the coarse-weight path over those pixels: mean gap **1.36 / 255**. Without a far image: no marker pixel.
* Real renderer (`RenderSceneImage`, forward) on a pack with a marker image: OpenGL and Metal 2364 marker pixels, near ground still green;
  Vulkan (MoltenVK) 0, as intended.
* `validate_embedded_shaders.py`: 65 GLSL shaders compile (the Vulkan landscape stages among them). `editor_help_audit.py --check`: clean.

## Not verified / not done

* **Textured ground.** Every GPU witness drew flat colours. That the slice means match what the arrays sample to is read from code
  (Metal uploads arrays with the asset's sRGB flag), not measured. If the arrays are missing at runtime but were there at export
  (or the other way round), near and far differ.
* **The export dialog and an exported game**: only the functions were driven (tests). 100 km was neither baked nor timed.
* **Windows**: nothing ran there. D3D11/D3D12 do not read the image (HLSL port open), Vulkan does not upload it. On the owner's
  RTX 4070 the picture is therefore unchanged until those two steps are done.
* **Progress**: the bake blocks the main thread like the other export bakes; `farProgress` exists and is tested but the dialog does not use it.
* Step 26 (reflection check must fail on a sky-only mirror) is untouched: it needs GL 4.3 compute, which no Mac has.

## Handoff

- **Topic / step:** 326. Step 25 (merge release/0.8.0) done, `cc1ea57f1`. The far image is new work from the owner, no board step yet.
- **Branch / commits:** `claude/landscape-weltmassstab-100km`: `cc1ea57f1` (merge), `6a89e8938` (far image), then the fixes and this doc.
- **Done:** everything under "Built", verified as above.
- **Decisions taken:** bake the tile shader, not the material (ring at the atlas edge otherwise); region = 8 x 8 tiles; one image
  (albedo + AO), the normal stays the far mesh's; far section before the HLOD section; cache under `Saved/`, never in the project's pack;
  all array slices or none in `landscapeFarPaintFor`.
- **Suggested, not taken:** a baked normal (second image) for shading detail finer than the far mesh; the editor showing the far image
  (the project's pack never carries one, only the shipped copy).
- **Traps:**
  * `far`, `near`, `small` are Windows macros: the accessor is `farImage()`, parameters `farTable`.
  * A filtered access inside the branch on the slot needs explicit gradients (`textureGrad`); an implicit one there is undefined.
  * `landscape_gpu_check` used one landscape object for several runtimes that all had generation 1. Fixed in the runtime, not the tool.
  * The whole world's image is resident (39 MB at 100 km, 16 texels a tile): marked `shortcut:` in `LandscapeFarMap.h`. The hash reads
    every stored tile byte on every export: `shortcut:` in `LandscapeFarBake.cpp`.
  * MetalRenderer binds RVT at fragment textures 18-21 since the merge; the landscape binds 20-30 before its own draws only.
- **Next steps, in order:**
  1. Vulkan: upload `farMips` (mipped `R8G8B8A8_SRGB`, linear clamp sampler at binding 13), set `texMaps[2]`; `landscape_gpu_check --only vulkan` then expects marker pixels (`takesFarImage`, and the list in `EngineCheck.cpp`).
  2. D3D11/D3D12: port the far branch to `kLandscapeHLSL` (t44, `SampleGrad`), upload, same checks. NN-WS03.
  3. One exported game by hand: "Bake far landscape" on, read the cook step's "Landscape far image: ..." line, export twice (second: 0 baked).
  4. Textured witness: scene 4 with a bound array (`--arrays`), mean gap of far against near.
  5. Thema 335 step 10 after PR 233: the tile world draws the landscape material / RVT; then a painter that photographs it replaces `landscapeFarWeightPainter`.
  6. Step 26 on NN-WS03. Still open from before: sculpt write-back (items 2-7), a streamed coarse world (the size cap), coloured cell proxies, cell streaming in the editor.
