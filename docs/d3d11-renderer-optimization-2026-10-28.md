# D3D11 renderer: CPU-side waste in the per-frame / per-draw path (2026-10-28)

Branch `claude/d3d11-renderer-optimierungen-9dc4b8`, cut from release/0.8.0 @ 31d6253c.
All code is in `src/HE_Rendering/src/Backends/D3D11/D3D11Renderer.cpp`.

## Handoff

**Done** (commit 3477c4b4, one commit for all groups — the edits interlock in `drawDC`
and were verified together, not split):
- 1 `MatDrawRec` / `matRec()` (~l.1431-1500): per-material cache of hash, baked variant, plain/instanced/G-buffer
  shader pointers, heTexP SRVs. Validated by string-compare of the sources, texture lists, content epoch
  (NOT by InvalidateMaterial alone: `ContentManager::syncMaterialInstance` swaps an instance's GLSL in place).
  Dropped by InvalidateMaterial, texture invalidation, shutdown.
- 2 Texture caches keyed by UUID (`graphTexById`, `graphTexArrById`, `graphTexByPath` for id-less, `uiTexCache`);
  weightmap memo per DrawScene (`lastWeights`); `resolveUITexture` without strings.
- 3 `flushMat()` + `matStateLive`: one bind / one restore per run of graph-material draws; bound-state tracking
  for VS/PS/IL/VB/IB/t2/t4-7/t14; HeParams Map skipped on identical 256 B (`lastParams`).
  Tessellation HS/DS are still unbound per draw.
- 4 `uploadClusters` reuses the first fill when the GI-mask flag is equal (both build and 6 uploads).
- 5 `bindObjectCB()` once per pass instead of two Set*ConstantBuffers per `uploadObject`.
- 8 persistent bone scratch (only the previous draw's tail is re-identity'd; the full 8 KB CB is still mapped
  per draw: bonesCB is also written by the world-preview path, so "unchanged" is not provable here).
- 10 `contributesAO` skip in the SSAO/reflection pre-pass (GL's reflection MRT shares that loop, so parity).
- 11 persistent opaque/transparent/deferred-forward lists, debug-line verts, SSAO kernel once (static).
- 13 sky-env bake: half conversion fused into the parallel body, persistent buffer (bit-identical).
- 14 bloom blur: two immutable CBs (`bloomBlurCB`), postFxCB re-bound after. 
- 15 `GpuPassScope` after the early-outs in EncodeDecals / renderUIPass (those rows no longer show up with 0 ms
  when there is nothing to draw).
- Test fix: `tests/test_culling.cpp` greps the D3D11 source for `uiTexCache.erase(id)` now.
- `scripts/d3d11_pixel_ab.py`: 10 witness scenes, capture + compare (see below).

**Verified**
- Build: HorizonEditor + he_tests, Release, MSVC (VS18), `C:/hd11b`, `-DDEPLOY_DIR=C:/hd11b/deploy`.
- he_tests (scratch APPDATA): 5949 cases, 5947 pass. The 2 failures were (a) my forward-slash `APPDATA` in a
  path-compare test (passes with a backslash APPDATA) and (b) the source-grep test above (fixed, passes).
- Pixels: `scripts/d3d11_pixel_ab.py` on D3D11 (HE_DUMP_RHI=D3D11, hidden window, scratch APPDATA, `HE_DUMP_NOSKY=1`
  because the sky drifts with SDL_GetTicks). Noise floor A0 vs A1 = **0 px** in all 10 scenes, then baseline
  (31d6253c build) vs new: **0 differing pixels, max delta 0** in all of: mat-forward, mat-deferred,
  mat-matte-bloom-fxaa, landscape-gi, water-shore, shadowinst-ssao, shadowinst-gi, manylights, taa-motion, ui.
  Not covered by a witness: tessellated displacement draws, instanced graph-material twins on a batch,
  skinned meshes, ribbons — the code paths for them were only reasoned about, not rendered.
- CPU time, Catania (`HEProject.heproj`, 72 draws, 387k tris, D3D11, hidden, 600 frames, `he_perf_capture.py`),
  baseline binary vs new, 3 runs each, p50 of the profiler scope:

  | scope | baseline | new |
  |---|---|---|
  | DrawScene | 2.25 / 2.32 / 2.28 ms | 0.24 / 0.29 / 0.32 ms |
  | Scene (inside) | 2.09 / 2.11 / 2.11 ms | ~0.1 ms |
  | Present | 0.18-0.24 ms | 2.2-2.5 ms |
  | Render | 2.59 / 2.62 / 2.68 ms | 2.54 / 2.61 / 2.73 ms |
  | frame CPU p50 | 2.95 / 3.00 / 2.98 ms | 2.81 / 2.77 / 2.78 ms (~-6 %) |
  | fps | 318-322 | 354-359 (~+11 %) |

  **Read this honestly:** this scene is GPU-bound (gpu p50 ~2.7 ms, unchanged by RenderScale 0.25 — it is
  not resolution-bound). The wait for the GPU moved from inside DrawScene (the driver blocking on a full
  queue, likely the Map(DISCARD) per draw) into Present; Render is the same, so the real win is the
  frame CPU p50 -6 % and fps +11 %, not the 2 ms in DrawScene. The scope drop is real but not "2 ms saved".
  I did not build a CPU-bound many-draw scene; the Catania project has only 72 visible draws.
  Not isolated per finding: the gain is the sum.

**Not verified:** hot-reload of a material edited in the Material Editor while running (the
string-compare validation is meant to cover it, no live edit was done); a run with GI shading active in the
cluster-reuse branch where the mask flag differs (the rebuild branch is the old code).

**Skipped, with reasons**
- 6 (resolve meshes once into a parallel vector): `resolveMesh` is one hash find per draw per pass and
  the passes take a `resolveMeshFn(UUID)`, not an index; changing all six pass signatures for ~30 ns x draws x
  passes was not worth the risk. 
- 7 (TAA velocity dense arrays / filtering): entity ids are sparse and the velocity of "unchanged" objects is
  not provably zero (camera motion); not touched.
- 9 (UI quad batching): needs a new instanced UI shader to keep draw order and blending; the string-key
  removal (2) is the only part done. `RSGetState` is already once per pass.
- 12 (redundant VB/IB/SRV sets in the built-in path and `drawDepthInstanced`): VB/IB/SRV0 tracking is done
  for the graph-material path only; the built-in path was left (needs the same tracking across 4 callers).
- Throttling the sky rebake (13) and rendering the tonemap straight into the viewport (14): both change the
  picture or the target chain; not done.

## Next steps
1. A CPU-bound many-draw scene (hundreds of graph-material draws, low GPU load) to measure DrawScene honestly.
2. Extend `d3d11_pixel_ab.py` with a skinned, a tessellated and a graph-instanced-batch witness.
3. 12 for the built-in path; 6 if the draw count gets large.
