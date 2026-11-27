# Tile world: far image baked in the editor, from the landscape material (topic 355, step 5, 27.11.)

## Handoff

**Done** (branch `claude/landscape-one-system`):

1. **Editor bake on a worker** (`ae6b126c1`): `src/HE_Editor/LandscapeFarBake.{h,cpp}`. Cache `Saved/LandscapeFar/<pack>.far` (the export's).
   Setting `EditorConfig::LandscapeFarBake`: Off / Manual (Tile Landscape ▸ Far Image ▸ Bake Far Image) / Automatic.
2. **Far image under the material, Metal** (`547bfe380`): `HE_LS_FAR` early-out in `kLandscapeMaterialFragmentHead`, texture 31.
3. **No view limit** (`98a27027c`, `444dad4d6`): the editor camera's far plane is 250 km (`EditorCamera.h`); it was 5 km while tiles load
   to ≥ 7.6 km, so the far image was never on screen. Framing keeps 4.5 km (`kFrameMaxDistance`).
   The slot table is toroidal and no shader checked the entry's owner: ground ~32 km out read a near tile. Every tile fragment
   shader (GL/Vulkan GLSL, MSL, HLSL, material head) now treats a tile ≥ half a window from the camera's tile as not resident,
   only when the world is wider than the table (keepAll worlds unchanged).
4. **Far image from the material** (`d2d547ac2`): `RvtLandscape::renderTileGroundImage` draws a region from above with the base colour
   layer view (`rvtTileGround`, `rvtSlot < 0`, `Capabilities::landscapeGroundImage`, Metal only). The worker asks, the main thread
   draws once the region's tiles are in the atlas (`askGround`/`serveGround`), 4×4 px a texel. Manual walks the streaming anchor
   (`tileAnchor`); Automatic paints only loaded regions, the rest stay stale (`stats.deferred`). The export keeps those regions
   (`farKeepPainterHash`). Witness switch `HE_LANDSCAPE_FAR_BAKE_NOW=1`.
5. **What stays is baked, the weather is live** (`2f9a7ffec`, `4564d8a1a`): the bake draws the material's FINAL base colour
   (`TileGround::farView`: RVT pair inlined, the Output's Base Color cut through one more Write node) under a dry, snowless sky
   (`renderTileGroundImage` zeroes wetness / snow / puddles of the environment for the render): painted layers incl. the Snow
   layer and the height snow are in it, the weather's snow and wetness not. Painter version 3. The far early-out lays the
   weather's Snow Cover on again as one colour mix (MF_WeatherSnow without height bias × Auto share, the material's Snow Color /
   Snow Max Slope found by their `// param:` names). `heLsFar` takes its gradient before the branch (no bright atlas-edge line).
6. **Camera flights as video** (`9a2c73f66`): `scripts/he_flythrough.py` + `src/HE_Editor/EditorFlythrough.{h,cpp}` (path JSON,
   realtime pacing, frames → MP4 via ffmpeg or `scripts/he_video_encode.swift`); `docs/headless-runs.md` has the recipe.
7. **Clouds jumped in flight** (`0306c3087`): the sky rebuilt its view rays from inverse(proj × view) with the camera's 38 km
   translation inside; `HE::SkyViewProj` (rotation only) for Metal / GL / Vulkan. Measured with `cloud_track.py` (here) on
   `--keep-frames` flights: straight flight at 38 km 23.6 → 0.2 px a frame. D3D11/12 still the old way (Windows).

**Verified**

- ctest 409/409; help audit; `landscape_gpu_check` metal / gl / vulkan exit 0; HLSL through glslang (PSMain, VSMain).
- 100 km Catania from 3000 m: `HE_LS_FAR` 0/1 differs in 182 782 px (was 4 with the 5 km far plane); aliased patches at the horizon gone.
  The old line "7 182 px at 1000 m, only the ground beyond the atlas" was most likely tiles still streaming, not the far image.
- Catania is flat (all heights 0) with one layer everywhere: any far image of it is one colour. Witness world instead:
  `docs/landscape-one-system-far-image-2026-11-27/far_testworld.py` (decoder: `far_to_png.py` there) → 40 km, 79×79 tiles, 900 m mountain, rock ring, snow layer, a letter F of dirt;
  migrated with `docs/landscape-one-system-baseline-2026-11-26/tools/migrate.cpp` (include fixed). Manual bake: 100 regions from
  the material in 91 s; the decoded `.far` has F and mountain in place (not mirrored/transposed); drawn beyond the atlas top-down and
  at eye level. With snow (5.): 100 regions in 47 s, the snow band continuous across the atlas edge top-down, flight video
  (`docs/landscape-one-system-far-image-2026-11-27/flight.json`: 4 keys, 24 s; 721 frames in 36 s). ctest 410/410 after 5. and 6.
  Weather Snow Cover 0.8 in the scene: the baked image unchanged; top-down far image vs the whole material everywhere: mean
  difference 1.43 levels, 2 302 of 1.9 M px off by > 30 (30.08 / 220 675 without the Auto-share factor).

**Not verified**

- Automatic mode with the material over a session (edit → only the touched, loaded regions); the 30 s timeout path.
- Frame times while a manual bake walks (the viewport shows coarse ground meanwhile; nothing covers it).
- Other backends: they keep the weight painter (no `landscapeGroundImage`), and the alias fix is not run on D3D.

**Decisions taken / open**

- The coarse quadtree patches are the "static mesh"; no mesh assets for the ground.
- Owner, 27.11.: the far image holds the Snow layer (static), not the weather's snow or wetness; far snow is its colour only.
  Wetness, puddles and trails are not drawn beyond the atlas at all. The far snow skips the relief height bias of MF_WeatherSnow.
- Texture changes of the material are not in the painter hash (only the graph key): re-bake by hand after a texture edit.

**Next steps**

1. Watch flights over a real project with `he_flythrough.py` (streaming pop-in at speed, the atlas-edge change in motion).
2. The same ground image read-back on GL/Vulkan, then Windows (D3D).
3. Step 6 (mesh HLOD per cell).
