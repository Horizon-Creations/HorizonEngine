# Editor viewport: FPS drop while flying, cause and fix (Thema 221, step 2)

Step 1 (`docs/editor-viewport-fps-camera-motion-2026-10-18.md`) could not reproduce the drop on
the Mac. This step started from the human's own data on NN-WS03 and named the cause.

## 1. What the human's logs and dumps say

`HorizonEngineBuild/Editor` (build of 18.10. 12:57 = release/0.7.0 @ c5202e42; copies kept in
`C:\Users\conno\he221-evidence`), project HEProject (Catania terrain: res 1025, 256 chunks):

| Source | Situation | FPS |
|---|---|---|
| `dumps/gi_off_d3d12.json` | D3D12, GI off, flying (W held) | ~246 (CPU 4.1 ms) |
| `HorizonEngine.log` f 31842..32159 | D3D12, GI on, only turning | ~105 |
| `dumps/go_on_d3d12.json` + log f 32253..32491 | D3D12, GI on, flying | ~21-25; `Render` 100-200 ms on every other frame, GPU 5 ms |
| `HorizonEngine.1.log` | GL, flying | ~470 `uploaded mesh 'terrain_chunk'` in 10 s (4481 / 1217 verts = LOD0 / LOD1 + skirt) |

The D3D12 backend had no scopes below `Render`, so the dump could only say "CPU, inside the
renderer, only while the position changes, only with GI".

## 2. Cause

1. `TerrainSystem::stitchSeams` (PR #162, Thema 190, `src/HE_Scene/src/TerrainSystem.cpp`) gives
   the finer side of every mixed-LOD seam a stitched copy. Whenever a chunk or a neighbour
   changes LOD (= the camera POSITION moves across a LOD ring; turning does nothing) it rebuilds
   that copy in place: `replaceStaticMesh` + `renderer->InvalidateMesh`.
2. GL, D3D11, D3D12 and Vulkan answered `InvalidateMesh` of any mesh in the software GI BLAS cache
   by dropping the WHOLE cache (concatenated node/triangle arrays, "no splice"). The next GI frame
   rebuilt the BVH of every shadow caster (~256 chunks), re-uploaded everything and, on D3D12/
   Vulkan, also built the DXR/VK BLAS of the new copies with GPU syncs. Metal keeps one
   acceleration structure per mesh, which is why the Mac in step 1 never dropped.
3. GL builds that cache also for **GI reflections** (`UpdateGIAccel`: `m_giEnabled ||
   m_giReflEnabled`), and the human's config has `GIReflectionsEnabled = True`: hence "GL drops
   with GI off too". D3D11/D3D12/Vulkan have no GI reflections, so only GI on.

The #162 author had flagged it (`docs/gi-landscape-tile-seams-fix-2026-10-13.md`, Traps) as "only
on LOD changes, not per frame" — with a flying camera and 256 chunks that is several per second.

## 3. Fix

`HE::GiBlasStore` (`src/HE_Rendering/include/HorizonRendering/GiBlasStore.h`, `src/GiBlasStore.cpp`,
test `tests/test_gi_blas_store.cpp`), used by all four backends instead of map + two vectors:

- `erase(id)` forgets one mesh, its range turns into dead space; nothing else is rebuilt.
- `compactIfWasteful()` (top of each `updateGiAccel`) MOVES the live ranges together once dead
  triangles outweigh live ones (and exceed 64 Ki): BVH indices are offset-relative, no rebuild.
- Uploads are tail-only between rewrites: D3D12 persistently mapped upload buffer, GL
  `glBufferSubData`, D3D11 `UpdateSubresource` with a box (buffer now DEFAULT, not IMMUTABLE),
  Vulkan memcpy of the tail; all with 1.5x headroom. A rewrite/growth gets a fresh buffer (D3D12
  retires the old one, Vulkan waits for the device first: rare).

Profiler scopes added (they make the next human capture explain itself):
`<Backend>::GiUpdateAccel / GiBuildBlas / GiUploadBlas / GiDropAccel / GiBuildHwBlas /
DrainInvalidations` (Backend = D3D11, D3D12, GL, Vulkan).

## 4. Measurements

NN-WS03 (RTX 4070, 32 cores), private copy of HEProject, the human's settings (deferred,
SSAO, occlusion culling, GI shadow quality 2), camera at the human's saved pose, hidden window
2000x1125, vsync off, 300 warm-up + 400 frames, `HE_PERF_CAM_FLY=1.0` (±240 m) vs still.
A = c9e7d522 + scopes only, B = + fix. Script: `C:\hw221\cap.sh` (wraps `scripts/he_perf_capture.py`).

| Backend, setting | A still | A FLY | B still | B FLY | BVH builds / drops per 400 FLY frames, A → B | CPU p95 FLY, A → B |
|---|---|---|---|---|---|---|
| D3D12, GI on (DXR) | 148.7 | **37.8** | 175.2 | **168.6** | 15130 / 59 → 71 / 0 | 151.8 → 17.0 ms |
| Vulkan, GI on (ray query) | 140.6 | **34.3** | 155.0 | **128.3** | 15130 / 59 → 71 / 0 | 154.9 → 14.2 ms |
| D3D11, GI on | 201.5 | **42.1** | 261.0 | **249.3** | 15130 / 59 → 71 / 0 | 142.2 → 5.6 ms |
| GL, GI off + GI reflections on | 98.0 | **29.3** | 87.1 | **86.1** | 15130 / 59 → 71 / 0 | 170.5 → 16.5 ms |
| GL, GI on + GI reflections on | | **27.6** | 82.7 | **78.3** | 15130 / 59 → 71 / 0 | 171.2 → 17.1 ms |
| GL, GI + reflections off (control) | | 116.1 | | 118.2 | – | 11.4 → 10.8 ms |
| D3D12, GI off (control) | | | | 267.0 | – | 5.5 ms |
| Vulkan, GI off (control) | | | 154.4 | 141.6 | – (`DrainInvalidations` 59× 3.6 ms) | 11.0 ms |

Still frames move ±15 % between runs on this shared machine (other bees build); compare FLY
with the still of the same side. What is left while flying, per backend (B):
- D3D12: `D3D12::GiBuildHwBlas` 71× avg 6.2 ms — the DXR BLAS of each new stitched copy, built on
  a one-shot list with two `waitForAllFrames()` (the p95 17 ms).
- Vulkan: `Vulkan::DrainInvalidations` 59× avg 3.8 ms — `vkDeviceWaitIdle` on every frame with a
  pending mesh invalidation (pre-existing, also with GI off), plus `GiBuildHwBlas` 71× 1.7 ms.
- All: `GiBuildBlas` ~1 ms per changed chunk (8 Ki triangles), `GiUploadBlas` < 0.1 ms p95.

## 5. Beyond FPS: the compaction path and the GI image

Long flights (2500 frames, C = B + a log line per compaction, `HE_SKY_TIME=100` pins the sky
clock, `HE_CAPTURE_FRAME=2700` writes the viewport): every backend logs `GI BLAS store compacted
(341 meshes, 1878400 triangles)` at f 1182 (GL also f 2360), logs otherwise clean (Vulkan's 5-7
errors are the ImGui-shutdown validation noise, same count before the fix). Hardware paths over
the whole flight: D3D12 223 fps, Vulkan 135 fps; D3D11 266, GL 73 (GI + reflections).

GI image at f 2700, after the compactions and ~450 tail appends, A (full rebuild every drop =
reference) vs C, bottom half (terrain, where the GI shadows are), mean |Δ| over 0-255:

| Backend (software BVH kernels) | A vs C | C vs C2 (noise floor) |
|---|---|---|
| GL (GI + reflections) | 0.001 | 0.001 |
| D3D11 | 0.345 | 0.290 |
| D3D12, `HE_GI_FORCE_SW=1` | 0.332 | 0.311 |
| Vulkan, `HE_GI_FORCE_SW=1` | 0.003 | 0.003 |

(The D3D sky still drifts with run time despite `HE_SKY_TIME` — A runs 3x longer — so only the
terrain band is compared; the sky reads no BVH.) That band does carry the GI: GL C with GI and
reflections off at the same frame differs from C by 14.6 (8 % of pixels by more than 24). Unit test: `tests/test_gi_blas_store.cpp` 5/5;
negative control (compaction without the node-offset update) fails it.

## Handoff

- **Topic / step:** 221, step 2 (human data → cause → fix → verify)
- **Branch / commit:** claude/editor-viewport-fps-brechen-bei-kamerabewegung-von-120-auf-3,
  merge of release/0.7.0 (c9e7d522, queen OK'd, request #129) + fix 2027daf8 + 488e9cab + this doc (pushed)
- **Done:** flying the camera with GI on (GL: GI or GI reflections on) no longer rebuilds every
  GI BVH; moving ≈ still on D3D11/D3D12/GL, Vulkan at 83 % of still (see Pending).
- **Files touched:** `src/HE_Rendering/include/HorizonRendering/GiBlasStore.h` + `src/GiBlasStore.cpp`
  (new), the four backends' `InvalidateMesh` drain / `updateGiAccel` / BVH upload (grep `giBlas`,
  `m_giBlas`), `tests/test_gi_blas_store.cpp`, both CMakeLists.
- **Verified:** sections 4/5 (A/B on all four backends, long flights through compaction, GI image
  A/B, unit test + negative control). Full he_tests (Release, scratch APPDATA): 5519/5524; the
  5 failures are the 3 gamepad cases (real pad on NN-WS03) and 2 `test_profiler.cpp` asserts
  (107, 215) that pass alone and fail in the full run with or without `test_gi_blas_store.cpp`
  (no backend is compiled into he_tests): not from this step, order-dependent in one monolithic
  he_tests run; not checked on release/0.7.0 itself (CI runs one ctest per file).
- **Not verified:** Metal-SW (same whole-cache drop in `MetalRenderer.mm` ~16695, not ported: no
  Mac compiler here); the human's own 120 Hz screen (all runs hidden window, vsync off); Linux.
- **Decisions taken:** fix the renderer policy, not the stitching (GI needs the stitched geometry,
  a per-chunk key cache would still evict = invalidate); compaction moves ranges instead of
  rebuilding; GiBlasStore shared + unit-tested instead of four hand copies.
- **Decisions suggested, not taken:** D3D12 `buildGiHwBlas` (and Vulkan's) record the BLAS on the
  frame list and retire geo/scratch instead of 2x `waitForAllFrames()` per new mesh (6 ms each,
  p95 17 ms while flying); Vulkan `processPendingInvalidations` retire list instead of
  `vkDeviceWaitIdle` per invalidation frame (3-4 ms, 59 of 400 flying frames, GI on or off);
  LOD hysteresis for terrain chunks (fewer re-stitches at all).
- **Pending:** the two sync items above (own A/B each); Metal-SW port of GiBlasStore (Mac).
- **Tools on NN-WS03 (not in the repo):** `C:\hw221\deployA` = before-fix deploy, `deployB`/`deployC`
  = fix; `C:\hw221\cap.sh` (one capture), `long.sh` (long flights + images), `ppmdiff.py`, `sc.py`
  (scope summary); private project copy `C:\hw221\proj`. Follow-ups can A/B from there without a rebuild.
- **Traps:** `he_perf_capture` runs at different FPS = different wall time → time-driven sky and
  clouds differ; pin `HE_SKY_TIME` before any image A/B. A killed `long.sh` keeps spawning
  `cap.sh` children — kill the bash tree. he_tests: scratch `APPDATA`.
- **Next steps:** 1. Human: fly on the new build with GI on, F9 dump — the `<Backend>::Gi*`
  scopes now name any rest. 2. D3D12 HW BLAS without CPU wait. 3. Vulkan retire list.
  4. Metal-SW on a Mac (`HE_GI_FORCE_SW=1` repro).
