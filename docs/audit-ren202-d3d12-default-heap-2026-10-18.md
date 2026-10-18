# REN-202: D3D12 static geometry in the DEFAULT heap (Thema 217, step 1)

Audit entry: `docs/audit/modul_rendering.md`, REN-202. Mesh vertex/index buffers (static
and skeletal) and the GI BVH node/triangle arrays were `D3D12_HEAP_TYPE_UPLOAD` buffers
for their whole life, so on a discrete GPU every vertex fetch and every GI ray step read
host RAM over PCIe.

## What changed

All of it is in `src/HE_Rendering/src/Backends/D3D12/D3D12Renderer.cpp`, in the block
"Static geometry in the DEFAULT heap (REN-202)" right after `createUploadBuffer`:

* `createStaticBuffer(capacity, data, dataBytes)` creates a DEFAULT buffer in COMMON and
  queues a copy (`queueBufferUpload`). `uploadMesh`, `resolveSkeletalMesh12` (all four
  buffers) and `uploadGiBlasArray` use it. The GI tail append (`from > 0`) is a queued
  copy into the live buffer now, not a memcpy into a mapped one.
* The copies go on a dedicated upload command list (allocator pool, each allocator reused
  once its batch's fence has passed). `submitToQueue(list)` flushes it (Close, Execute,
  Signal a fresh value on the shared monotonic fence) and then executes `list`. All ten
  former `cmdQueue->ExecuteCommandLists` sites go through `submitToQueue`, so no draw can
  run before its mesh copy. ImGui's own multi-viewport submits never read meshes.
* No barriers: buffers promote COMMON → COPY_DEST on the copy, decay back to COMMON at the
  end of the upload list's ExecuteCommandLists, and the first draw/dispatch promotes them to
  the read state it needs.
* Staging comes from `HE::FrameUploadRing` (the backend-free ring of REN-302): 4 MB chunks,
  released by the batch's fence. A copy bigger than a chunk, or past the 16 MB ring budget
  of one batch (a level load), gets its own staging buffer that dies with the same fence, so
  the ring stays bounded.
* Kill-switch `HE_D3D12_STATIC_UPLOAD_HEAP=1`: the old upload-heap buffers, same binary. It is
  also the fallback when the upload list or a DEFAULT buffer cannot be created.
* Completed batches are swept at the top of every `Render()` (`retireCompletedUploads`), so a
  scene that uploads nothing more does not keep a level load's staging alive.
* Witness: `D3D12Renderer: static geometry in the DEFAULT heap — first upload batch ...` at the
  first flush, and a totals line at Shutdown (buffers, MB, batches, ring/dedicated copies,
  copies into a live buffer, fallbacks).

Not done, from the audit's REN-202 list: (2) sub-allocating meshes from one big VB/IB pool
(each mesh is still two committed resources with 64 KB granularity). REN-212's texture
staging still uses one upload buffer per texture; it could move onto the same ring.

## Verification (NN-WS03, RTX 4070, Release, private deploy `C:/hw217/deploy`)

Harness: `scripts/d3d12-static-heap/cap202.ps1` (one D3D12 HE_DUMP capture, scratch APPDATA,
private FXC cache, `-UploadHeap` = the kill-switch) and `diff202.py` (BMP diff, per band).
DEFAULT = this change, UPLOAD = same binary with `HE_D3D12_STATIC_UPLOAD_HEAP=1`.

| Witness | Result |
|---|---|
| `HE_DUMP_SECTIONTEST=1` (3-slot sphere), `HE_GPU_DEBUG=1` | witness: 4 buffers in DEFAULT, 0 fallbacks. Sphere disc (119 433 px): DEFAULT vs UPLOAD mean\|d\| = 0.0000, 0 px > 2. Whole frame differs only in the sky bands, exactly as much as two DEFAULT runs (noise floor). Debug layer: the same 2 pre-existing ClearRenderTargetView warnings on both sides. |
| GI, SW kernels: `LANDSCAPELAYERS GI GIREFIT HE_GI_FORCE_SW`, top-down | DEFAULT vs UPLOAD 0.161, DEFAULT vs DEFAULT 0.168, 0 px > 8. Debug layer quiet. The refit landscape doubles the BVH, so this run takes the rewrite branch (no copy into a live buffer). |
| GI, hardware (DXR 1.1) path, same scene | DEFAULT vs UPLOAD 0.116, DEFAULT vs DEFAULT 0.116. Debug layer: 3 pre-existing `InitialState UNORDERED_ACCESS ignored` notes (DXR scratch), identical on both sides. |
| GI tail append (`from > 0`) | temporary build with 4× headroom (reverted): 2 copies into a live buffer, DEFAULT vs UPLOAD 0.084. **Negative control**: same build with the tail copy skipped: 4.42 mean\|d\|, 315 310 px > 8, so the witness does see the tail data. |
| Negative control, static meshes | temporary build whose flush never executes the upload list: the sphere is gone (11.8 mean\|d\|), and **the debug layer reports nothing**. Only the pixels and the source pin catch a missing copy. |
| Skeletal | **not verified at run time.** `HE_DUMP_SECTIONTEST=skinned` draws nothing on D3D12 on BOTH sides (`draws=0`), and the same on D3D11 and Vulkan; only GL draws it. That is pre-existing. `HE_DUMP_SECONDARY=1` + `HE_WORLD_PREVIEW_DUMP` (RenderWorldPreview, which also resolves skeletal meshes) uploads no skeletal mesh either (2 buffers = the cube). The skeletal buffers are covered by build + pin only. |
| `tests/test_culling.cpp` "D3D12 static geometry lives in the DEFAULT heap ..." | passes (35 assertions, incl. the per-frame sweep in `Render()`); it fails (3 ≠ 2) when one raw `ExecuteCommandLists` is put back (checked). |
| full `he_tests` (Release, scratch APPDATA, `HE_NET_LOOPBACK_ONLY`) | 5523/5528 passed. Failures: 3 gamepad E2E (real Xbox pad, known), 2 `EngineProfiler` cases that pass alone (9/9) and fail only in the full run (shared singleton state). |

## Measurement (inconclusive under load)

`HE_DUMP_FOLIAGETEST=100000 FOLIAGEMESH=sphere HE_DUMP_GPUTIME=300` (4.9 M triangles, 18
draws), interleaved, medians in ms: DEFAULT 8.44 / 42.50 / 11.70, UPLOAD 7.96 / 43.57 / 10.78.
During the runs the human's game had the RTX 4070 at 95-98 % (10.4 of 12 GB VRAM), so the
round-to-round spread (8 → 42 ms) is a factor of 5 and buries any heap effect. The witness is
also the wrong shape for this question: one small sphere mesh, so its vertex data sits in L2
whatever the heap. **No gain or loss is claimed.** A fair number needs an idle GPU and a scene
whose distinct static geometry is far larger than L2 (36 MB), e.g. many unique imported meshes
or a dense landscape, plus GI SW traversal for the BVH part.

## Handoff

- **Topic / step:** 217 (Audit-Optimierung D3D11/D3D12/OpenGL, Stufe 3), step 1 of 2 (REN-202)
- **Branch / commit:** `claude/audit-optimierung-d3d11-d3d12-opengl-backend-stufe-3`, see the
  REN-202 commit on top of `cad90a7a` (pushed: yes)
- **Done:** D3D12 static meshes, skeletal meshes and the GI BVH arrays live in the DEFAULT heap,
  filled by copies on a dedicated upload list that `submitToQueue` runs before every submit.
- **Files touched:** `D3D12Renderer.cpp` block "Static geometry in the DEFAULT heap (REN-202)"
  (after `createUploadBuffer`), `uploadMesh`, `resolveSkeletalMesh12`, `uploadGiBlasArray`,
  `Shutdown` (`releaseStaticUploads`), the 10 former `ExecuteCommandLists` sites;
  `tests/test_culling.cpp` (last case); `scripts/d3d12-static-heap/*`.
- **Verified / not verified:** see the two sections above. Not verified: skeletal at run time;
  any performance gain. (The DXR BLAS build keeps its own temporary upload buffer, untouched.)
- **Decisions taken:** separate upload list instead of recording on the frame list (resolveMesh
  runs inside preview/capture/frame lists and at Initialize; no `cl` threading); no barriers
  (implicit buffer promotion/decay); `FrameUploadRing` reused, 4 MB chunks, 16 MB ring budget
  per batch, larger copies get their own staging buffer; kill-switch env kept for A/B.
- **Not done (audit REN-202 point 2):** sub-allocation of meshes from one DEFAULT pool
  (each mesh is still two committed resources). REN-212 texture staging could share the ring.
- **Traps:** C: on NN-WS03 ran full (0 bytes) during this step; check free disk before builds/captures. The D3D12 debug layer does NOT flag an unfilled DEFAULT buffer (negative control).
  `Copy-Item` of a backup keeps its old mtime: ninja then says "no work to do" and the deploy
  keeps the experiment binary; touch the restored file.
- **Next steps:** 1. Step 2 of the topic (REN-103/601, REN-603). 2. When the GPU is idle,
  re-measure with a geometry-heavy scene (above). 3. Find out why D3D11/D3D12/Vulkan draw
  nothing for `HE_DUMP_SECTIONTEST=skinned` (pre-existing, not part of this topic).