# The tile ground on every backend, the black tiles of Catania view 5, where the extra GPU time goes (topic 355, step 3, 2026-11-26)

Follows `docs/landscape-one-system-baseline-2026-11-26.md` (step 2). Branch `claude/landscape-one-system`, Apple M5 on mains power, Metal /
MoltenVK / macOS OpenGL 4.1 run here; Windows (D3D11, D3D12, real Vulkan, OpenGL 4.6) only audited, from the code and from
`docs/landscape-world-scale-windows-handoff-2026-11-21.md` sections 9 to 14 (NN-WS03, 22.11.). **The table in the Handoff of
`landscape-tile-material-state-2026-11-22.md` and section 8 of the Windows handoff are older than this one.**

## 1. The table

**V** = ran and judged by a witness (where, when). **B** = in the code and claimed by `Capabilities`, no run of mine or of a doc shows it.
**M** = not built / not possible. Capabilities: `landscapeMaterial`, `landscapePassMask` (`kLandscapePass*`), `landscapeRvt`
(`IRenderer.h:28`, `:297`); Metal `MetalRenderer.mm:20951-20953` (all), GL `OpenGLRenderer.cpp:15726`, Vulkan `VulkanRenderer.cpp:1401`,
D3D11 `D3D11Renderer.cpp:10107`, D3D12 `D3D12Renderer.cpp:14859` (both behind `landscapeD3dPassesOn`, on under Windows).
"Win §n" = the Windows handoff, section n. "today" = `landscape_gpu_check --only <b>` on this Mac, 26.11., exit 0 on metal, gl, vulkan.

| Tile ground | Metal (M5) | GL macOS 4.1 | GL Windows 4.6 | Vulkan MoltenVK | Vulkan Windows | D3D11 | D3D12 |
|---|---|---|---|---|---|---|---|
| Drawn with its material | **V** today: 112 047 / 111 955 px (fwd / def) | **M**: 16 fragment units, the material variant needs 24 (`m_maxFragmentSamplers`) | **V** Win §9, §10 | **V** today: 113 481 / 114 598 px | **V** Win §9 | **V** Win §10: 115 250 / 109 033 px | **V** Win §10, same numbers |
| Casts into the shadow layers | **V** today: box under the ground 150/255 vs 223 | **V** today: 150 vs 223 (fixed blend) | **V** Win §9 | **V** today: 4 vs 176 (fwd), 62 vs 212 (def) | **V** Win §9: 140 vs 203 | **V** Win §9: 141 vs 203 | **V** Win §9, no debug layer ever run |
| Takes sun shadow (with material) | **V** today: 4 683 px | **M** (fixed blend lights nothing from the maps: tool prints `NOT CHECKED`) | **V** Win §9: 6 894 / 4 631 px | **V** today: 7 034 / 6 904 px | **V** Win §10 | **V** Win §10: 4 561 / 4 551 px | **V** Win §10 |
| GI (pre-pass, ray structure) | **V** today: 113 vs 218; **not exercised in Catania** (no mesh, `GIAccel` 0.00 ms) | **M** (GL < 4.3) | **V** deferred 4 913 px; forward faint, not claimed (Win §11, §12) | **V** today: 145 vs 204 | **V** Win §11: 5 514 px | **V** Win §11: 5 303 px | **V** Win §11 (DXR; the software kernel never ran) |
| SSAO position pre-pass | **V** today: 26 841 px | **M** (not in the mask) | **V** Win §11: 56 170 px deferred, 116 forward | **B** off on macOS (`landscapeVkSsaoOn`); today 0 px | **V** Win §9: 57 739 px | **V** Win §10: 62 577 px | **V** Win §10 |
| G-buffer (deferred) | **V** today: 108 213 of 111 945 px | **M** | **V** Win §11 | **V** today: 109 889 of 114 598 px | **V** Win §11: 108 568 px | **V** Win §11 | **V** Win §11 |
| Velocity (TAA) | **B** built; `EncodeLandscapePass` kind 4; the live-viewport witness (needs `--arrays`) was not run; no `taaPrevViewProjAt` for the origin hop (Win §14) | **M** | **V** Win §13: 1.53 / 4.05 | **B** claimed, not re-run | **V** Win §13: 2.14 / 5.60 | **V** Win §13: 2.20 / 5.73 | **V** Win §13 |
| Virtual texture on the ground (`landscapeRvt`) | **V** state doc sessions 3-4 (181 pages, 3.5/255); pages painted in every Catania capture today | **M**: RVT off, "16 fragment texture units, the lookup needs 26" (run log) | **V** Win §12 (Mountains arrays only) | **B** claimed; Catania capture draws, RVT not judged | **V** Win §12 | **V** Win §12 | **V** Win §12 |
| Far image, fixed blend | **V** today: 2 364 px marker | **V** today (forward, far image scene) | **B** Win §9 exit 0, line not quoted | **V** today: 2 364 px | **B** same | **B** same (HLSL branch since 22.11.) | **B** same |
| Far image **while the material draws** | **M** | **M** | **M** | **M** | **M** | **M** | **M** |
| Resolve position camera-relative (this step) | **V** 3 fills, views 1, 2, 4, 5 | **B** fill changed; GL 4.1 draws the ground with the fixed blend, so no witness | **B** fill changed | **V** fill changed, view 5 | **B** fill changed | **M** open: `D3D11Renderer.cpp:9386` | **M** open: `D3D12Renderer.cpp:14065` |
| Ground caster bias, sun cascades (this step) | **V** `MetalRenderer.mm:8829` | **B** 27.11.: `renderDepthLayer` sets `glPolygonOffset(2, landscapeCasterDepthBias(...))` around `DrawLandscapeDepth` in the sun's cascades and restores `(2, 4)`; `landscape_gpu_check --only gl` exit 0, stripes not judged (the fixed blend on GL 4.1) | **M** open | **V** `VulkanRenderer.cpp:8850` | **B** same code | **M** open (`D3D11Landscape` / `D3D12Landscape::drawDepth`; D3D12 sets slope 2 at `D3D12Landscape.cpp:260`) | **M** open |

Reading notes. (a) "Far image while the material draws" is missing everywhere by construction: the material's landscape variant has bindings
51-57 only (slot table, three tile weight pages, three coarse pages; `MaterialShaderLibrary.cpp`, `kLandscapeMaterialFragmentHead`);
only the fixed blend (`kLandscapeFS`, its HLSL / MSL ports) has the far branch (`uTexMaps.z`). (b) The Windows column is what NN-WS03 reported
on 22.11. (Debug, RTX 4070); nothing there was re-run for this step. D3D12 never ran under the debug layer, no Windows Release run, the Forest
arrays are missing there (Win §12), the editor by hand never. (c) The Metal GI witness uses a box under the ground; **the Catania tile scene
has no mesh, so on Metal no GI pass runs at all there** (baseline raw: `GIAccel`, `GIShadow`, `Reflections` 0.00 ms in every tile column, 2.4 ms in the old one).

## 2. The black tiles of view 5 (found, fixed, shown)

**What it was.** Not tiles, not the far image, not the RVT: the **deferred resolve placed the ground metres off** (the GPU read it 3.5 m too low) and the ground, which
casts into the sun's cascades, shadowed itself in the last cascade's wedge (stair-stepped = the cascade's texels).
Chain of evidence (all on the baseline's tile column, view 5 = tile coordinates 70 000, 300, 70 000, yaw 2.0, pitch -0.1; 6 s a run):

* only on the deferred path (forward and `HE_LANDSCAPE_PASSES=0`: none); the G-buffer's base colour, normal, roughness views are complete;
* with the ground drawn into the shadow layers one at a time, only layer 2 (the far cascade) makes it; with its patches filtered by level, only level 0;
* a project shadow `minBias` of 0.004, or `distance` 120, removes it; resolution 4096 does not;
* a shader probe in `heCsmShadow` (temporary, reverted) read, inside the wedge: the receiver's `worldPos.y` **-3.5 m** (the ground is at 0), `p.z - cd` +0.0054 of a 681 m depth range = 3.7 m;
* the cause: the resolve computes `invViewProj * clip` in float. The editor has no floating origin (render world origin 0), the camera was 70 km out;
  the inverse then holds numbers of camera / near in its last columns and the GPU's product cancels them. The same float arithmetic on the CPU
  (`tools/recon_error.cpp`, header `FarOriginPrecision.h`): ground pixel 530-800 m away, error in y **-1.3 m at 70 km, -6.9 m at 60 km,
  +14 m at 90 km, -0.3 m at 30 km, -0.05 m at 1 km** (the GPU read -3.5 m at 70 km); camera-relative: 0.03 to 0.35 m everywhere. Why it looked random: the sign and size depend on
  the camera's float digits; only a negative y puts the receiver "below" the caster.

**Fix** (`934972f45`, `7a714d14d`; header `HorizonRendering/FarOriginPrecision.h`):
1. `ResolveUniforms::camOrigin` (+16 bytes; every backend sizes the block with `sizeof`), the two resolve shaders add it after the divide;
   `HE::setResolveCameraRelative` fills the inverse of `viewProj * translate(camera)` (double) and the camera. Metal (3 fills), Vulkan and GL call it;
   **D3D11 / D3D12 keep `camOrigin` 0 = the old absolute inverse until NN-WS03 switches them** (one call each, lines above).
2. With the position right, the ground's own acne showed: the ground's depth is rasterised from float32 world coordinates 30+ km out, noisy at the scale of the
   receiver bias (Catania view 1 got stripes it did not have; view 2's baseline already had them). The meshes of a cascade keep the bias they have (Metal had **none**,
   although the other backends' comments say it does; GL / Vulkan / D3D11 / D3D12 use constant 4 / slope 2); the ground in a sun cascade takes
   `HE::landscapeCasterDepthBias` = 1.5 x `slopeBias` x (cascade + 1) of depth on top, Metal `setDepthBias`, Vulkan `vkCmdSetDepthBias`.
   View 1, bias x 0.5 / 1 / 2: 4 342 / 725 / 632 px of stripes against 109 893 without.

**Witness** (`tools/dark_px.py`: dark pixels of the lower half, "solid" = 5 x 5 all dark so grid lines do not count):

| picture | before | after |
|---|---|---|
| Metal view 5, `tiles-v5-before.png` / `tiles-v5-after.png` | solid dark **51 507** px, dark 80 618 | solid dark **0**, dark 13 822 (= the forward picture, grid lines only) |
| Vulkan (MoltenVK) view 5, `tiles-v5-vulkan-before.png` / `-after.png` | 963 horizontal dark runs (wedge of stripes) | 0 runs (2 169 with the reconstruction fix alone) |
| Metal view 1 against the baseline picture, px differing by > 30 | - | 723 (109 749 with the reconstruction fix alone) |
| Metal view 2 (`tiles-v2-after.png`) | the baseline has black stripes bottom left | gone |
| Metal views 3, 4 against the baseline pictures (baseline r1 and r2 are identical) | - | 114 px (grid lines); 4 733 px: view 4's cloud-shadow blotches, blocks before, smooth now (the same wrong position fed them) |

`landscape_gpu_check --only metal` after both commits: exit 0, every line as before (material 112 024 px, box shadow 4 683, ground casts 150 vs 223, GI 113 vs 218, SSAO 26 841,
G-buffer 108 214 of 111 945, far image 2 364). `--only vulkan`: exit 0. `--only gl`: exit 0. New test `tests/test_resolve_reconstruct.cpp` (2 cases, with the negative control that the absolute inverse is
> 0.5 m off at 60-90 km); the whole `he_tests` run after the last code commit: see the Handoff (Verified).

**Left, said plainly.** The root of the noise is that the editor renders a 100 km world without a floating origin: every vertex transform at 50+ km is float32 with a large translation.
The two fixes above remove what showed (the wedge, the stripes); a mesh at 70 km still has the same depth noise (a flat caster seen at grazing angle would stripe the same way),
and SSR trace, fog, clouds, underwater, decals still use `inverse(viewProj)` in float on every backend. The real fix is an editor floating origin or a camera-relative vertex stage for all geometry
(the baseline's "centre the world on migration" is now a correctness item, not a taste).

## 3. Where the extra GPU time and RSS go

Catania, Metal, deferred, 3200 x 1800, the editor of this tree, mains, load 1.9 to 2.6. Detailed mode (`gpuPassPercentiles` p50, 400 frames); "whole" = whole-frame GPU p50.
The 26.11. numbers are with the fix (no change from the baseline: view 1 8.42 vs 8.40, view 4 11.58 vs 11.59 ms whole, G-buffer 5.59 vs 5.58 / 10.78 vs 10.80).

| view | old column (baseline) | tiles, default | tiles, fixed blend (`textureGroup` 99, no material) | tiles, RVT off |
|---|---|---|---|---|
| 1 whole / biggest passes | 4.59: GBuffer 1.90, Reflections 1.52, GIShadow 0.49 | **8.42**: GBuffer 5.59, Scene 2.13 | **5.20**: Scene 4.21 (the ground forward), GBuffer 0.08 | not run |
| 4 whole / biggest passes | 5.36: GBuffer 2.77, Reflections 1.47, GIShadow 0.50 | **11.58**: GBuffer 10.78, Scene 0.32 | **4.11**: Scene 2.68, Bloom 0.77, GBuffer 0.14 | 11.96: GBuffer 11.17 |
| RSS max, view 4 | 1 649 MB | 2 135 - 2 158 MB | 1 390 MB | 1 885 MB |

* **GPU**: the cost is the landscape *material* in the G-buffer pass, not the tiles, the atlas, the patches or the extra passes. Taking the material away (fixed blend) leaves 4.1 - 5.2 ms,
  below the old column's whole frame (the old one runs 2.4 ms of GI passes the tile column does not run: not like for like). The material ground is +3.2 ms at view 1 and +7.3 ms at view 4
  (many more ground pixels at view 4). The tile ground reads a slot table and three weight pages per fragment instead of one weight texture (state doc: 14.1 vs 11.1 ms at 1600 x 900, in place).
  The RVT pages barely help here: view 4 11.96 ms in place against 11.57 from pages (0.4 ms), because the camera-centred volume is not what a 2 km high camera sees.
  CPU: `Landscape` 0.15 ms, `RvtLandscape` 0.1 ms, the frame is vsync-pinned: not the cost. **No Metal fix made**: nothing in these numbers is waste outside the material's own cost; the lever is
  "RVT tiers" (step 4 of the topic), which shrinks what the material evaluates in place.
* **RSS**: +770 MB at view 4 against fixed blend = **+495 MB the material path** (graph textures, pipelines; with RVT off) **+250 to 270 MB the RVT pool for the tile ground**. The tile atlas
  (budget 350 MB) is in the fixed-blend 1 390 MB too and is *not* the difference: the old column's 1 649 MB holds an RVT pool and the terrain meshes. Device (GPU) memory: not measured (the profiler's `vramUsedMB` is 0 on Metal).
* The hitches (64 per 600 frames on views 4, 5) were not looked at; the A/B did not show them in the detailed runs (400 frames).

## 4. macOS OpenGL: the gap stays open, with the reason

macOS GL 4.1 reports `GL_MAX_TEXTURE_IMAGE_UNITS` 16 (the run log: "16 fragment texture units, the lookup needs 26"). The landscape material variant adds seven images to a material's (the Auto Landscape
material then uses 22, `OpenGLRenderer.cpp:15724`; the check asks for 24), the virtual texture lookup 26. `landscape_gpu_check --only gl` today: exit 0, fixed blend, the ground casts 150 vs 223 /255, far image OK, GI "NOT CHECKED: this OpenGL has no GI (below 4.3)".
Closing it means merging the three tile weight pages and the three coarse pages into two arrays and trimming the material's own images by eight: a change to the material's binding model, outside this step. macOS GL is the one
backend where the tile ground stays the fixed blend; nothing else is missing there that the hardware allows.

## 4a. The gate's two findings, resolved (27.11., owner's follow-up)

**macOS OpenGL 4.1, material on the ground: re-scoped, not a gap of this topic.** The limit is the driver's (16 fragment texture
units, frozen since macOS 10.14: Apple deprecated OpenGL and ships no later version), the material variant needs 24 and the RVT lookup
26. Fitting under 16 would mean cutting the material's own images by eight for one deprecated backend, which every other backend would
pay for in its binding model. Decision: on macOS OpenGL the tile ground is drawn with the fixed blend (with its far image, shadows and fog,
all verified above); the Mac's material path is Metal. The table's **M** for macOS GL in the material, RVT and pass rows means "not on
this driver", and `landscape_gpu_check --only gl` stays the witness that the fixed blend keeps working there.

**Far image while the material draws: moved into step 5 (HLOD in the editor), with the design.** It is not impossible, but it does not
fit the current binding model, so it is not a gap this audit could close:
* Native Metal: the material landscape variant's fragment stage has no texture slot left (`MaterialShaderLibrary.cpp`, the MSL pins:
  tile and coarse weight pages on 25-30, "the last free ones"; all seven landscape images already use inline samplers because the
  stage has no sampler slot either). The far image needs one more texture.
* The way in: the three coarse weight pages (bindings 55-57) become one `sampler2DArray` with three layers. That frees two slots on every
  backend; one takes the far image (binding 58, a constexpr linear sampler with mips on Metal). In `main`, a fragment whose tile is not in
  the atlas (`heLsSlot == 65535u`) and lies beyond the RVT window then writes the far image's colour and AO as its base colour, with the
  geometric normal and a fixed roughness, and returns before the graph: exactly what the fixed blend does today, and it removes the cost
  step 4 measured (the full material evaluated over the coarse weights beyond the 4 km window from 200 m up).
* Because the head (`kLandscapeMaterialFragmentHead`) is shared by all five backends, the change touches every backend's landscape
  binding; Metal and MoltenVK are witnessed here, D3D11 / D3D12 / real Vulkan on NN-WS03.
That is the far tier step 4 asked for, and step 5 bakes the far image the editor would then draw. Step 5's text names it.

## 5. Open for NN-WS03 (Windows)

1. Switch the D3D resolve fills to `HE::setResolveCameraRelative(ru, viewProj, m_renderWorld.camera.position)`: `D3D11Renderer.cpp:9386`, `D3D12Renderer.cpp:14065`. Verify with the Catania view-5 recipe (below) at 70 km.
2. The ground's caster bias in the sun's cascades on GL (`glPolygonOffset` before `DrawLandscapeDepth`), D3D11 and D3D12 (`DepthBias` in the ground's depth state): `HE::landscapeCasterDepthBias(slopeBias, cascade)` is in depth-buffer units of 2^-24 (a float depth).
3. After this branch: `landscape_gpu_check --only d3d11|d3d12|vulkan|gl` once (the resolve block grew by 16 bytes; Metal, Vulkan, GL sized from `sizeof`, D3D11 `makeCB(sizeof)` and D3D12 `alignUp(sizeof)` by reading, never run), and `he_tests --source-file=*test_resolve_reconstruct*`.
4. Still open from the Windows handoff: D3D12 under the debug layer, a Release run, `--arrays` with the Forest arrays, the editor by hand (Win §5), the unit test for the HLSL shader contract with the new `camOrigin` member.
5. The far image while the material draws (all backends, design) and the velocity witness on Metal / MoltenVK stay missing.

## Handoff

- **Topic / step:** 355, step 3 of the "one landscape system" topic. **Branch / commit:** `claude/landscape-one-system` @ the commit that adds this file (fix commits `934972f45`, `7a714d14d`); pushed: see the Hive result.
- **Done:** the per-backend table (section 1); the black wedge of view 5 found, fixed on Metal, Vulkan, GL (camera-relative resolve position) and on Metal + Vulkan (ground caster bias), before/after pictures in
  `docs/landscape-one-system-baseline-2026-11-26/`; the extra GPU time and RSS explained with A/B numbers; the macOS GL gap recorded with its reason.
- **Files touched:** `HorizonRendering/FarOriginPrecision.h` (new: `setResolveCameraRelative`, `landscapeCasterDepthBias`); `material/MaterialShaderLibrary.h:ResolveUniforms::camOrigin`;
  `MaterialShaderLibrary.cpp` (two `HeResolve` blocks + two reconstruct lines); `MetalRenderer.mm` (3 resolve fills, `:8829` cascade bias); `VulkanRenderer.cpp` (`:11629`, `:8850`); `OpenGLRenderer.cpp:14750`;
  `tests/test_resolve_reconstruct.cpp`. The bias comment in `D3D11Renderer.cpp:5110` ("Metal: setDepthBias") was wrong until now.
- **Verified:** `landscape_gpu_check --only metal|vulkan|gl` exit 0; Catania views 1, 2, 4, 5 on Metal and view 5 on Vulkan before/after; full `he_tests` on the final code (after `7a714d14d` and the widened test bounds): 6 605 cases, 0 failed, 21 skipped; the A/B GPU and RSS numbers above. **Not verified:** D3D11 / D3D12 / real Vulkan / GL 4.6 with this change
  (the code path is the same helper; nothing run); the Metal velocity pass; GI in a mesh-less tile world; device memory; the editor by hand; a Release `he_tests`.
- **Decisions taken:** `camOrigin` is a new field, zero = the old behaviour, so a backend not switched renders as before (the queen asked that Windows only be audited); the caster bias is the ground's only (the meshes keep theirs) and for the sun's cascades only;
  no Metal performance change (nothing was waste). The commit trailer is `Claude Sonnet 5.5`, the model that wrote it (the assignment text named Haiku 5.5).
- **Decisions suggested, not taken:** an editor floating origin or a camera-relative landscape/mesh vertex stage (the root of the noise); a shadow-distance cut in `heCsmShadow` (the last cascade is applied out to its sphere, 585 m with a 250 m distance: the wedge lay at 400+ m; changing it touches the five backends' built-in shadow copies).
- **Pending, in order:** (0) the caster bias (1.5 x slopeBias x (cascade + 1), 2.4 m at cascade 2) was tuned on a flat world: re-check ridge-onto-valley ground shadows on the hilly Catania the baseline asks for; (1) NN-WS03 items of section 5 (the D3D resolve HLSL gets `camOrigin` through SPIRV-Cross; the WARP tests in `test_material_graph` prove the cbuffer layout there); (2) tile world GI without a mesh (the Catania scene runs no GI pass on Metal); (3) RVT tiers: the material, not the tiles, is the GPU cost; (4) hitches of the tile column (64 per 600 frames) not investigated.
- **Traps:** `he_perf_capture.py` throws stderr away (a debug `fprintf` needs a file); `--scene` copies over the project's startup scene and stays there (restore it); the editor's `HE_DUMP_GBUFFER` shows the G-buffer's colour, not the lighting; a Metal debug value in `heLight.shadowBias.z`
  reaches only the forward fill; `cd` in a Bash call sticks to the next call.
- **Next steps:** 1. run `tools/run_view.py` + `tools/dark_px.py` on Windows for D3D (set `--rhi D3D11`). 2. D3D fills + GL / D3D bias. 3. Step 4 of the topic (RVT tiers) starts from section 3's numbers.
