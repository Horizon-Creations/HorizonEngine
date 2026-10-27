# Landscape streaming, LOD and rendering cost: static audit (Catania report)

27.10.2026, branch `release/0.7.0`. Static read only: nothing built, run or measured, no source edited. The Catania project
exists only on the user's Mac, so every cost below is **structure + HUD arithmetic**, not a measurement. Evidence ("E") is a
file:line I read; guesses ("G") are marked. Does not repeat `world-streaming-baseline-2026-10-06.md` (generic scene streaming,
which already says terrain is never streamed by distance) or `landscape-performance-regression-*` (the material/heBombGridV fix).

## 0. Result in six lines

1. The user's HUD (D3D11, 2560x1440: 60 FPS, GPU 11.4 ms, 15 draws, 122k tris) does **not** show a geometry/streaming problem.
   Draw count and triangle count are small: chunks are culled and LOD works (E: HUD). The top GPU pass is `GIShadow` 5.3 ms.
2. `GpuTimerReap` 8.7 ms CPU is a **profiler artefact**: `gpuDetailed` makes `gpuTimerEndFrame` call `gpuTimerReap(slot, block=true)`,
   which does `Flush()` and spins on `GetData` (E: D3D11Renderer.cpp:2263-2266, 2312-2314, 2180-2187). It is the CPU waiting for
   the GPU, not CPU work. Real CPU is about 20.9 - 8.7 = 12 ms (G: arithmetic, assumes the wait is the whole scope).
3. The scalable cost is the **GI acceleration structure**, which scales with the number of shadow-casting chunk *entities*
   (every one, unculled), not with what is on screen: D3D11 traces with a **linear instance loop per ray** (E: HlslSources.h:883-914
   `giSceneAnyHit`/closest-hit, `for (i < uGiCount.x)`), and Metal **rebuilds the whole TLAS every frame** (E: MetalRenderer.mm:8670-8680, 8790-8815).
4. Terrain meshes are rebuilt only on events (sculpt/dirty, tessellation slot change, seam re-stitch), not per frame (E, section 1).
   The event cost is high though: CPU mesh build on the main thread + GPU buffer re-create + CPU BVH rebuild per replaced mesh.
5. On **Vulkan every mesh invalidation does `deviceWaitIdle()`** (E: VulkanRenderer.cpp:8412-8418): a re-stitched or re-tessellated
   chunk stalls the whole GPU. D3D11/GL/Metal free the one mesh and re-create lazily.
6. There are **no counters** for mesh uploads, bytes, invalidations, BVH builds, stitch/tess rebuilds or GI instance count (section 5).
   Until they exist, nobody can say which of the above the Mac actually pays.

## 1. TerrainSystem / LODSystem: what rebuilds a mesh

Chunk grid (E: TerrainSystem.cpp:47-61): `computeGrid(res)`, chunks of 64 cells, `chunksPerSide` clamped 1..32, but
`kTerrainMaxResolution = 1025` (TerrainComponent.h:16) so at most 16x16 = 256 chunks per landscape, 4 LODs (65/33/17/9 verts per side).
Per chunk: one entity with `LODComponent` + `MeshComponent` + own `MaterialComponent`. LOD distances: `chunkWorld * 6 * lodDistanceScale * 2^k` (:358-370).

| Trigger | Where | What happens | Frequency |
|---|---|---|---|
| `tc.dirty` / `tc.regionDirty` (load, sculpt, noise slider, resolution snap) | updateTerrains :552-690; `buildChunk` :649 | all L LOD meshes of each touched chunk regenerated, `replaceStaticMesh` + `InvalidateMesh` per LOD (:340-370). Full rebuild = 256 chunks x 4 LODs; sculpt brush uses a rect (:613-621) so only a few chunks | event |
| Tessellation slot (`tessellationFactor>=2`, `tessellationDistance>0`; **off by default**, :810-812) | updateTessellation :783-927, `fillTessLevel` :287 | nearest chunks within `D*1.25` get a 257^2 (factor 4) mesh: **2 builds/tick, 16 slots/terrain** (:147-148). Release at `D*1.5` or when rank > 16 (:896-901) | event, but see churn below |
| Seam stitching | stitchSeams :930-1088 | when a chunk's key (level + coarser-neighbour cells per edge + corners, :233-243) changes: `stitchTerrainChunkEdges` copies the whole chunk mesh, `replaceStaticMesh` + `InvalidateMesh` (:1052-1071) | each time a LOD ring boundary crosses a chunk; changes the key of the chunk **and its 4 neighbours** |
| LOD switch | LODSystem.cpp:99-104 | only sets `meshAssetId` + `mesh.dirty` when the chosen level changed; no rebuild | event |

Answers to the specific questions:

* **LODSystem is distance-only with no hysteresis** (E: LODSystem.cpp:80-97, plain `dist <= maxDistance`). `mesh.dirty` is set only on an
  actual change (:99-104), not every frame. I found no reader of `MeshComponent::dirty` in `RenderExtractor.cpp` or its header
  (grep), so it is cosmetic for the renderers; they key on `meshAssetId` (section 2).
* **A flip near a LOD threshold is cheap for the mesh itself** (both meshes stay in the GPU cache) **but not for the seams**: the flip
  changes the stitch key of up to 5 chunks, each of which re-copies and re-uploads a mesh (E: :1048-1071). `cc.stitchKey` holds one
  key, so going back and forth between two stitch configurations rebuilds every time; only returning to the *plain* level re-uses
  the stitched mesh (comment :1076-1078). A camera hovering at a ring boundary therefore re-stitches repeatedly (G: no measurement,
  structure is clear).
* **Tessellation slot churn** (E: :894-926): chunks are ranked by distance every tick; rank > 16 is emptied (`emptyTessLevel` =
  `replaceStaticMesh(empty)` + `InvalidateMesh`, :274-281) while the new rank <= 16 chunk is built. Two chunks near rank 16 that swap
  order as the camera moves rebuild each other. The 1.25/1.5 distance margins only protect the distance test, not the rank cap.
* **`computeTerrainHeightField(tc)` runs on the first build of every tick that builds** (E: :911-914; TerrainMeshGenerator.cpp:72-93):
  a 1025^2 copy (4 MB) for a sculpted landscape, or a full 1M-sample fbm for a noise one (G: tens of ms). Cache it per landscape
  until `tc.dirty`.
* **Per-frame CPU that scales with chunk count** (all main thread, E): `updateTerrains` material-sync pass over every chunk with a
  linear terrain lookup and `paramOverrides` compare (:490-515); `updateTessellation` view over all chunks, `worldPositionOf` only when
  tessellation is on (:849-870); `LODSystem::update` one view, parent matrix composed once per sibling run (LODSystem.cpp:11-70);
  `stitchSeams` allocates `unordered_map` + a `C*C` slot vector **every frame** and keys every chunk (:960-1086). With 256 chunks each
  is small (G: well under 1 ms together); they are not the 9 ms in the HUD. They have profiler scopes `Terrain`, `TerrainTess`, `LOD`,
  `TerrainSeams` (SceneSystems.cpp:120, 161-165), which the user did not quote: **ask for those four numbers**.

## 2. Backend reaction to `InvalidateMesh` / changed `meshAssetId`

`InvalidateMesh(id)` only queues the id; the drain runs at the top of the frame. GPU mesh caches are keyed by mesh UUID and
never bounded (no eviction): every LOD of every chunk that was ever drawn stays resident (G: memory, not measured).

| Backend | Drain | Cost per invalidated mesh | Evidence |
|---|---|---|---|
| D3D11 | `processPendingInvalidations` erases `meshCache`, `skeletalMeshCache`, the mesh's GI BLAS; next draw calls `resolveMesh` | on a miss: CPU re-interleave of the mesh (loose assets) + 2x `CreateBuffer` IMMUTABLE + albedo SRV lookup. No pooled/updatable buffer | D3D11Renderer.cpp:5915-5931, 5977-6026, 4947-4965 |
| OpenGL | `glDeleteVertexArrays/Buffers`, erase, BLAS per mesh | same pattern, re-create on next draw | OpenGLRenderer.cpp:11216-11235 |
| Vulkan | `deviceWaitIdle()` first, then free vbuf/ibuf/albedo image | **full GPU stall per drain**, comment calls it a follow-up (D3D12 has a retire list) | VulkanRenderer.cpp:8412-8447 |
| Metal | `CFBridgingRelease` vertex/index buffer **and `blas`**, erase | hardware BLAS rebuilt on next GI use | MetalRenderer.mm:16961-16985 |
| D3D12 | per-frame retire list with descriptor slot countdown | not read further | D3D12Renderer.cpp:4339-4342 |

Nothing is uploaded or reallocated per frame for an unchanged mesh id (E: `resolveMesh` returns the cache hit first, D3D11 :5979).
Chunk meshes are not freed and re-created on a plain LOD flip. They **are** replaced in place (same UUID) on sculpt, tess slot change
and re-stitch, which is a full GPU re-create plus a CPU BVH rebuild.

## 3. Per-frame cost that scales with chunk count (render side)

* **GI instance list is unculled by design** (E: D3D11Renderer.cpp:3387-3395, comment "castsShadow only, UNCULLED"). Terrain chunks cast
  shadows (TerrainSystem.cpp:383-388), so all 256 chunks, plus every other caster, are instances every frame. Per instance:
  `glm::inverse(transform)`, and **`giInstanceSB` re-created via `CreateBuffer` + SRV every frame** (:3437-3440, comment "IMMUTABLE re-create is fine").
* **D3D11 TLAS is a linear loop** (E: HlslSources.h `giSceneAnyHit`/closest hit loop over `uGiCount.x`, each iteration two matrix
  multiplies and a BLAS root test). GI Shadow cost per ray grows with instance count. With 256 chunks plus props this is the best
  explanation of `GIShadow` 5.3 ms when only 15 draws are visible (G: not profiled, no instance count available).
* **BLAS per distinct mesh id**: `resolveRange` builds a CPU BVH on first sight of every mesh id (E: :3381-3388, `BuildGIBvh` :3281).
  Each LOD switch to a never-seen LOD, each stitch and each tess replace costs a main-thread BVH build (scope `D3D11::GiBuildBlas`
  exists). A factor-4 tess mesh is about 131k triangles; 16 of them put about 2M triangles into the BLAS store (G: arithmetic).
* **Metal**: `EncodeGIAccelBuild` calls `m_extractor.extract` again (:8670), releases and rebuilds `m_giTlas`, instance buffers and
  colour buffers **every frame** over all unculled casters (:8672-8815). `extract` has 6 call sites in the file (ExtractFrame,
  EncodeGIAccelBuild, EncodeSSAO, EncodeScene, EncodeGBuffer, EncodeFrame); how many run in one frame was not traced. D3D11 extracts
  once (D3D11Renderer.cpp:7012). This is the most plausible reason Mac is worse than the Windows HUD (G).
* **Main scene and CSM are culled** (E: D3D11 shadow views culled per cascade on workers, :7455-7475; 15 visible draws). Not the problem.

## 4. CellStreamer.cpp and terrain

No relation. `grep -i "terrain|chunk"` in `src/HE_Scene/src/CellStreamer.cpp` finds nothing; the cell plan lists terrain as
Resident ("Terrain/Terrain-Chunk bleibt", `entity-cell-streaming-plan-2026-10-10.md:221`) and explicitly excludes terrain streaming
(:41, :529). So a landscape never gets fewer chunk entities or GPU meshes by distance; all 256 chunks stay as entities, casters and GI instances.
A distance-based chunk streamer (despawn/merge far chunks) does not exist.

## 5. Instrumentation: what exists, what is missing

Exists (E): `ProfSceneCounters` (EngineProfiler.h:95-106: entities, lights, particles, emitters, bodies, scripts, streamingInFlight);
`ProfRenderStats` (draws, tris, visible, total, VRAM, :109-122); `FrameGpuStats` (IRenderer.h:321-336, adds `occlusionCulled`, per-pass
times); scopes `Terrain/TerrainTess/LOD/TerrainSeams` (SceneSystems.cpp), `D3D11::GiBuildBlas`, `GiUploadBlas`, `GiUpdateAccel`,
`GpuTimerReap`, `Vulkan::DrainInvalidations`. `HE_LOG_DEBUG` lines for leaked tess/stitch meshes (TerrainSystem.cpp:799, 945).
`RenderExtractor::fullExtractCount()` (RenderExtractor.h:269).

Missing: mesh upload count/bytes, invalidation count, BVH builds and triangles, GI instance count, stitch/tess/chunk rebuild counts,
LOD switches, mesh-cache size/bytes, number of `deviceWaitIdle` stalls, extractions per frame on Metal.

Minimal set (per frame, reset in `beginFrame`, shown next to draws/tris):

| Counter | Side | Increment at |
|---|---|---|
| `terrainChunkBuilds`, `tessBuilds`, `tessReleases`, `stitchBuilds` | scene | TerrainSystem.cpp `buildChunk` call :649; `fillTessLevel` :287; `emptyTessLevel` :274; stitch :1063-1068 |
| `lodSwitches` | scene | LODSystem.cpp:99 branch |
| `meshReplaceBytes` | scene | at each `replaceStaticMesh` (vertices+indices bytes), same four places |
| `meshUploads`, `meshUploadBytes` | renderer | D3D11 `uploadBuffers` :4947 (and the GL/Vulkan/Metal/D3D12 equivalents) |
| `meshInvalidations`, `gpuStalls` | renderer | `processPendingInvalidations` :5919; Vulkan drain :8418 |
| `blasBuilds`, `blasTris` | renderer | `BuildGIBvh` :3281 |
| `giInstances` | renderer | `giInstanceCount` after :3396 (Metal: `count` ~:8760) |

Plumbing: add a `ProfMeshStreamStats` next to `ProfSceneCounters` (EngineProfiler.h:95) filled in `pushProfilerSceneCounters`
(SceneSystems.cpp:178); renderer fields next to `FrameGpuStats::drawCalls` (IRenderer.h:325) and `FrameCounters` (D3D11 :2173).
Plain integers, no atomics needed on the main thread.

## 6. Ranked suspects

| # | Suspect | Est. cost | Confidence | Basis |
|---|---|---|---|---|
| 1 | GI shadow trace and accel build scale with all unculled chunk casters (D3D11 linear TLAS loop; Metal per-frame TLAS + re-extract) | GI Shadow 5.3 ms is the top GPU pass; Mac likely more | Medium (structure E, magnitude G) | HlslSources.h, Metal :8670-8815 |
| 2 | Vulkan `deviceWaitIdle` per invalidation drain | whole GPU frame per re-stitch/tess/sculpt frame | High that it stalls, unknown how often | VulkanRenderer.cpp:8418 |
| 3 | Seam re-stitch cascade at LOD ring crossings (5 chunks x copy + GPU re-create + BVH), no hysteresis | spikes of a few ms while moving | Medium (G: no frequency data) | TerrainSystem.cpp:1048-1071, LODSystem.cpp:80 |
| 4 | Tessellation (if enabled in Catania): 2x 257^2 main-thread builds/tick, field recompute, rank-16 churn, large BLAS | spikes, 2M BLAS tris | Medium; only if tess is on | TerrainSystem.cpp:147-148, 894-926 |
| 5 | Per-frame O(chunks) CPU in 4 terrain systems incl. `stitchSeams` heap allocs | under 1 ms | Low as a problem | :960-1086, :490-515 |
| 6 | Unbounded GPU mesh cache (all LODs resident) | memory, not time | Low | D3D11 :5979 |
| not a suspect | `GpuTimerReap` 8.7 ms, `mesh.dirty`, CellStreamer, main-scene draws/CSM | | High | sections 0-4 |

## Measurement: D3D11, user's HEProject (27.10.2026, NN-WS03)

`he_perf_capture.py --rhi D3D11`, editor build `C:/hw237/deploy/Editor` (26.10.), 2000x1125 hidden window, vsync off, GI/detailed profiler
off, project copied without `Export/`. Terrain 2000x2000 m, res 1025, 256 chunks. Caveat: not verified that this project is Catania.

* Constant 72 draws / 387k tris / 260 objects: no draw-count growth.
* CPU is almost entirely the D3D11 `Render` scope (14.8 s of 15.3 s self time); it has no sub-scopes. `Terrain`+`LOD`+`TerrainSeams` < 0.1 ms/frame.
* Run 1 (cold shader cache): Render spikes 265/278/492 ms in the first ~200 frames, then steady 2-3 ms (~350 fps). GPU ~2 ms.
* Run 2 (warm cache): no spike >100 ms, but Render still 3-30 ms during the first 600 frames.
* Start: TerrainSystem rebuilds 256/256 chunks, `tickWorld` 120 ms.
* Not measured: GI on, camera motion, 1440p, mesh upload counts (no counter exists), cause of the spikes (shader compile vs upload is a guess).

### Counters added and second measurement (27.10.2026)

Counters (`MeshStreamCounters` in `EngineProfiler.h`, written per frame into `frames[].stats` of the profile JSON): `chunkBuilds, tessBuilds,
tessReleases, stitchBuilds, lodSwitches, meshReplaceBytes` (TerrainSystem.cpp, LODSystem.cpp), `meshUploads, meshUploadBytes, meshInvalidations,
blasBuilds, blasTris, giInstances` (D3D11Renderer.cpp: `uploadBuffers`, `processPendingInvalidations`, `BuildGIBvh`, GI instance build). `gpuStalls`
exists but nothing increments it yet (Vulkan drain, D3D12/GL/Metal not wired: D3D11 only). Build `C:/hw238` (Release, `-DDEPLOY_DIR=C:/hw238/deploy`).
Runs: 600 frames after 400 warm-up, D3D11, vsync off, hidden 2000x1125 window, `HE_PERF_CAM_FLY=0.6` for the moving camera, GI via `--set GlobalIlluminationEnabled=true`.

| run | stitch builds | mesh uploads (bytes) | invalidations | BLAS builds (tris) | GI instances | CPU excl. Present p50 / p95 / p99 / max |
|---|---|---|---|---|---|---|
| A static, GI off | 0 | 0 | 0 | 0 | 0 | 3.4 / 3.8 / 4.3 / 5.1 ms |
| B fly, GI off | 60 | 60 (14.9 MB) | 60 | 0 | 0 | 2.1 / 3.2 / 3.7 / 4.2 ms |
| C fly, GI on | 66 | 70 (17.0 MB) | 62 | 70 (596k) | 257 | 2.2 / 4.3 / 6.9 / 20.6 ms |

Findings:
1. **No mesh-upload problem.** A static camera uploads nothing. A moving one re-stitches a chunk about once per 10 frames (about 250 KB each, 1 upload
   + 1 invalidation per stitch, `chunkBuilds`/`tessBuilds` 0). 90 LOD switches per 600 frames. Frames with an upload cost 3.8 ms against 3.3 ms without.
2. **GI on:** all 257 instances go to the GI scene (the audit's unculled-caster suspect is confirmed as a count), each re-stitch triggers one BLAS rebuild
   (about 8.5k tris, cheap). CPU is about 3 ms per frame higher than with GI off; the p99/max tail (6.9 / 20.6 ms) is the only place GI shows on the CPU.
3. **Every hitch (200 ms - 2.3 s) is the `Present` scope, in frames with zero uploads/stitches/BLAS builds.** The first measurement saw them as `Render`
   because the old build had no `Present` scope. With a hidden window, vsync off, `Present` blocks on the swapchain/DWM: a harness artefact until shown
   otherwise, not a landscape cost. The CPU work excluding `Present` stays below 5 ms (GI off).
4. **The 20.9 ms CPU / 5.3 ms GIShadow in the user's HUD is not reproducible here:** it was a visible window at 2560x1440 with the detailed GPU timer
   (blocking `GpuTimerReap` 8.7 ms). 15 draws / 260 objects there match run C (GI on), so the user's scene was GI-on.

Not measured: GI shadow cost on the GPU at 1440p (needs a visible window and detailed capture on a real run), Vulkan/D3D12/GL/Metal counters.

## Handoff

- **Topic / step:** Catania landscape performance, static audit (no number measured), step 1 of 2
- **Branch / commit:** `release/0.7.0` @ 8d638d58, doc uncommitted (pushed: no)
- **Done:** read-only audit answering the five questions with file:line evidence and a ranked suspect list.
- **Files touched:** only this doc. Key code to open: `TerrainSystem.cpp:894-926` (tess slots), `:1048-1071` (stitch), `LODSystem.cpp:80-104`,
  `D3D11Renderer.cpp:3386-3440` (GI instances), `:2263-2314` (blocking reap), `HlslSources.h` `giSceneAnyHit`, `MetalRenderer.mm:8670-8815`,
  `VulkanRenderer.cpp:8412-8447`.
- **Verified:** by reading only (Read/grep). The blocking-reap condition, the linear TLAS loop, the per-frame instance buffer re-create,
  the Vulkan `deviceWaitIdle` and the absence of terrain in CellStreamer are all directly in the code.
- **Not verified:** every cost figure. No Catania data, no profile, no Mac. How many `extract` calls run per Metal frame. D3D12 invalidation
  path. Whether Catania has tessellation on, how many chunks and casters it has, what the GI instance count is. That `MeshComponent::dirty`
  has no reader outside the files I grepped (RenderExtractor, HE_Rendering).
- **Decisions taken:** none in code. **Suggested, not taken:** (a) GI instances culled to a radius around the camera plus a
  cheap far set, or terrain merged to one coarse GI caster per N x N chunks; (b) hysteresis band in `LODSystem` (switch in at `d`, out at
  `d*1.1`); (c) stitch key cache of 2 entries per chunk, or stitch only the finer side lazily; (d) cache `computeTerrainHeightField`
  per landscape; (e) replace the Vulkan `deviceWaitIdle` by a retire list as D3D12 has; (f) persistent Metal TLAS with refit.
- **Pending:** counters (section 5), then a real Catania capture on the Mac.
- **Traps:** do not read the `*Renderer.*` files whole. `gpuDetailed` capture inflates CPU by the GPU time; compare CPU numbers with a
  non-detailed capture. `he_tests` wipes the real %APPDATA% config (use a scratch APPDATA).
- **Next steps:** 1. Ask the user for a Mac HUD/profile with `Terrain`, `TerrainTess`, `LOD`, `TerrainSeams` scopes, GI on/off, and whether
  tessellation is on. 2. Add the section 5 counters (small, no behaviour change) and capture Catania with GI off vs on to confirm suspect 1.
  3. Only then pick from the suggested fixes, starting with GI caster culling/merging and LOD hysteresis.

**Update 27.10.2026 (counters + measurement):** see the second measurement above. Next: wire `gpuStalls` (Vulkan `deviceWaitIdle` drain) and the upload/BLAS
counters into D3D12/GL/Vulkan/Metal; run a visible-window GI-on detailed capture at 2560x1440 to attribute GIShadow; consider GI caster culling by
distance (257 instances). Changes are uncommitted: EngineProfiler.h/.cpp, TerrainSystem.cpp, LODSystem.cpp, D3D11Renderer.cpp.
