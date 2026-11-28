# Landscape: new textures not visible, glossy Grassland, optional displacement (2026-11-04)

## 1. "The textures are not visible in my HEProject landscape"

Cause: the EngineContent sync only downloads files that are MISSING from `%APPDATA%/HorizonEngine/EngineContentCache`.
A file the server replaces under the same name stayed on the old cached copy for ever. HEProject's terrain is
`groupMix` (Mix material `0x4A4`), and its cache still held the pre-upload Mix and Mountains arrays (111 MB / Oct 9, the
Mix arrays have the same size as the new ones, only the hash differs); `content_sync_put verify` against that cache
showed all 15 files `BAD`.

Fix (commit `02d8668b`): `ContentSync/CacheFreshness.{h,cpp}`. After each manifest refresh a cached `.hasset` whose size or
FNV hash differs from its manifest entry is removed (so the on-demand download fetches the current one); hashes are
remembered in `<cache>/.cache-hashes.json` (and recorded when a download lands), files that are queued or downloading are
left alone. `tests/test_content_cache_freshness.cpp`. **It runs in the editor you build after this commit**; the editor
that is running now is the old binary. To see the new textures before that: close the editor and delete the stale files
from the cache (they download again when used): `T_Landscape_{Albedo,Normal,Mask}_Array.hasset`,
`T_Landscape_Desert_Albedo_Array.hasset`, `T_Landscape_Mix_{Albedo,Normal,Mask}_Array.hasset`.

## 2. "The landscape is reflective on the Grassland layer"

Cause: Poly Haven's roughness maps of wet mud are genuinely low. Bare Soil v1 `brown_mud_03` averages 0.52, v3
`brown_mud_dry` 0.53 (v2 `brown_mud_02` 0.95); the old server arrays already carried `brown_mud_03` (Mix slice 9 had
G 0.53 before and after). With sun and sky specular that reads as a mirror on ground with no wetness to explain it.
The automatic puddle overlay is separate (roughness 0.05 on water, ×0.5 on the wet rim) and was not touched.
Same effect, smaller: Mountains Dirt v2 / Rock v3, Forest Floor v3, Mossy Rock v2, Sandstone v2 (all ~0.52).

Fix: `landscape_tex_gen ... --min-roughness F` floors the roughness of REAL texture files (mask G). Repacked with 0.75;
only the Mask arrays change (Albedo and Normal arrays are byte-identical, `cmp`): the five Mask arrays (Mountains,
Forest, Grassland, Desert, Mix) were uploaded and verified (size + manifest hash), the manifest merged (99 entries).
Mix slice 9 (Grassland Bare Soil) now has roughness 0.75. A lower value brings the wet look back; per-material
roughness as a parameter would need a free parameter slot (see 3).

## 3. Optional displacement: prototype

The engine already tessellates materials with a connected Displacement pin (Thema 207, hardware-checked on four
backends). The landscape material did not use it. `buildAutoLandscapeGraph(view, group, mixed, displacement)` (new
last argument, default false = the graph byte for byte as before) wires, for the Lit view, the blended texture height
(mask B of the final surface) − 0.5, times `kAutoLandscapeDisplacementMetres` (0.5 m), to pin 9, with tessellation 8x
from 15 to 90 m and a ±0.25 m clamp. `landscape_tex_gen <dir> --material --displacement [--group G | --mix]` writes
such a material; nothing shipped or uploaded uses it.

Verified (D3D11, RTX 4070, camera `CAMX=-62 CAMY=326 PITCH=-24`, Mountains witness):
* the displaced material builds a tessellated shader set (`D3D11Renderer: built a tessellated material shader set`);
* with `HE_TESSELLATION=0` the same material renders **bit-identical (MD5)** to the shipped one (negative control);
* tessellation on changes 0.7 % of the pixels by more than 12 levels, concentrated on the rock band
  (`docs/img-landscape-slope-variants/displacement-prototype-off-on-d3d11.jpg`: off, on, 6x amplified difference, crop).
  At the 100 m+ witness distance the effect is subtle (the factor fades out at 90 m); no close-up was judged by eye.
* `test_auto_landscape_variants` new case (off = same JSON, on = pin linked + 4 nodes, debug views untouched);
  `test_material_graph`, `test_engine_materials`, `test_landscape_groups`, `test_material_tessellation` green.

Not done / for the real version:
* **Parameter budget**: Mountains has 15 of 16 `HeParams` slots, Desert 16/16. A strength and a switch need one slot:
  pack them into an existing Vec4 (Snow Look / Sand Look style) or take the static-switch route (a "Displacement"
  switch is free, strength constant).
* **LOD cracks**: terrain chunks of different LOD have T-junctions, displacement widens them (the design doc lists it).
* Shadows (CSM) and GI do not see the displaced surface; the lit normal is the texture's, not the displaced geometry's.
* The Vulkan quadtree path of the world-scale landscape (Thema 326) and the far-landscape bake are separate draw
  paths: tessellation there was not looked at. Metal/GL4.1 and the other D3D12/Vulkan/GL frames not run for this.
* Cost: the domain stage evaluates the whole blended-height chain (bombed array reads at mip 0) per tessellated vertex.
* A displacement-enabled material per group needs its asset regenerated and uploaded (small files).

## Handoff

* **Topic / step:** landscape follow-ups after the slope-variants step. Branch `release/0.8.0`; commits `02d8668b`
  (cache freshness) and the one holding this doc (not pushed).
* **Done:** 1 cache freshness (code, test); 2 roughness floor (tool, five Mask arrays uploaded + verified);
  3 displacement prototype (builder flag, tool flag, witness, test).
* **Files:** `src/HE_ContentSync/{include/ContentSync/CacheFreshness.h,src/CacheFreshness.cpp,src/EngineContentSync.cpp}`,
  `src/HE_Tools/src/LandscapeTexGen/main.cpp` (`gMinRoughnessByte`, `gDisplacement`),
  `src/HE_Core/{include,src}/MaterialGraph/AutoLandscapeMaterial.*` (`displacement`), `tests/test_content_cache_freshness.cpp`,
  `tests/test_auto_landscape_variants.cpp`.
* **Decisions taken:** roughness floor 0.75 baked into the Mask arrays (user); displacement as a prototype only (user).
* **Decisions suggested, not taken:** a settings switch / parameter for roughness and displacement strength; a
  puddle-free "dry" Grassland default; deleting the stale cache files for the running editor automatically.
* **Traps:** a changed `HE_API` signature (here `buildAutoLandscapeGraph`) makes an older `HorizonEditor.exe` fail to
  start (unresolved import) until it is relinked: rebuild `HorizonEditor`, not only `he_tests`; a running
  `content_sync_put` locks `HorizonContentSync.dll` and fails the next build; `$home` is read-only in PowerShell.
* **Next steps:** 1. rebuild and restart your editor (cache refresh + new Mask arrays arrive); 2. decide where the
  displacement strength lives (Vec4 slot) and look at it close up on a real project; 3. LOD-crack check on a chunked
  terrain; 4. Metal/GL frames.

## 4. Parameter limit 16 -> 32 (`kMatMaxParams`)

`MaterialGraph/MaterialParamLimits.h` (new, light header): `kMatMaxParams = 32`, `kMatParamFloats = 128`,
`kMatParamBytes = 512`. Every place that hard-coded the 16-vec4 / 64-float / 256-byte HeParams block now uses them:
the generated `vec4 v[N]` (fragment and WPO preamble), the per-draw staging arrays in D3D11, D3D12, Vulkan, OpenGL,
Metal and `RenderExtractor`, the GL UBO, the D3D11 constant buffer, the Vulkan and D3D12 per-draw rings (stride and
descriptor range = `kMatParamBytes`, still a multiple of 256) and the OpenGL change cache. The shipped baked materials
carry the GLSL text, so `M_AutoLandscape{,_Forest,_Grassland,_Desert,_Mix}`, `Water` and `DisplacementDemo` were
regenerated (`landscape_tex_gen --material [--group G]`, `--mix`, `mat_gen`); assets baked earlier keep declaring
`v[16]` and still work (a bigger buffer behind a smaller declared block). Parameter slots: Mountains 15/32, Desert 16/32.

Verified: `test_material_graph` 123/123, `test_engine_materials` 15/15 (SPIR-V, GLSL 4.1/ES/4.3, HLSL/FXC and WARP
D3D11/D3D12 reflect `HeParams` at `kMatMaxParams` x 16 B), `test_landscape_groups`, `test_auto_landscape_variants`,
`test_material_tessellation`; Desert witness on D3D11, D3D12, Vulkan and OpenGL: **byte-identical (MD5) frames** between
the old 16-slot editor and the new 32-slot one. Not verified: Metal (the same mechanical edit, no Mac), and a material
that actually USES a slot >= 16 on a GPU (the generator test covers the text, the Desert uses slots 0..15).

## 5. End to end on a copy of HEProject (private APPDATA, the editor built from this tree, real server)

* Seeded the cache with the stale Mix arrays: after the manifest refresh the editor logged
  `Cached '...Mix_*_Array.hasset' is out of date (the server has a newer one): removed` for all three, then downloaded
  them when the landscape needed them (about 4 minutes for ~1 GB), no manual cache cleaning.
* The terrain (groupMix, Mountains base) renders with the textures; `terrain_paint` on layers 2 (Dirt) and 3 (Rock)
  changes the ground (`docs/img-landscape-slope-variants/heproject-e2e-paint-layers.jpg`: before, after, close).
* **Group shares (layers 8..11, Forest / Grassland / Desert) are not painted with `terrain_paint`**: it refuses them
  by design (`TerrainPaint::paint`, `isReservedLayer`), the Landscape tool's group brush (`paintGroup`) writes them.
  That brush was not driven here (no MCP tool for it); `test_terrain_tools_ui` / the group paint tests cover it.
