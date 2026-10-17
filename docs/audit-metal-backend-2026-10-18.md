# Audit-Optimierung: Metal-Backend (Stufe 3)

Thema 216, from the performance audit (PR #161, `docs/audit/modul_rendering.md`,
`docs/audit/AUDIT.md` Stufe 3). Steps: 1 REN-302/402 upload ring, 2 REN-404/405/406
TLAS/BLAS dirty + asynchronous BLAS build, 3 REN-301 Pimpl.

## Step 1: REN-302/402, every per-frame upload through the `FrameUploadRing`

The ring (`FrameUploadRing.h`, `MetalRenderer::UploadTransient`) already served instance
matrices, the clustered-light lists and debug lines. Three hot paths still made a fresh
`MTLBuffer` per object or batch and frame; they now take a ring slice:

| Path | Before | Now |
|---|---|---|
| Skinned bone palette (`EncodeSkinnedObjects`, also `RenderWorldPreview`) | 128-matrix identity fill, copy, `newBufferWithBytes` 8 KB per object | `UploadBonePalette`: the pose plus identity up to `GpuSkeletalMesh::boneSlots` (highest joint id + 1, computed at upload) |
| ParticleGraph batches (`DrawParticleGraphBatches`) | `std::vector` + `newBufferWithBytes` per batch | instances written straight into the slice |
| Trail ribbons (`CollectRibbonDraws`) | VB + IB `newBufferWithBytes` per trail | two slices; `TPDraw::vbufOffset`, `indexOffset` |

Left alone on purpose: the GI per-frame tables (SW instance buffer, TLAS instance/colour/
land-index buffers, mesh-pointer buffers, landscape table, `.mm:8342-8620`). They are
members bound by many later kernels and exactly what step 2 rewrites into persistent,
dirty-flagged buffers; a ring slice dies with its frame, so moving them now would be undone
there. The previews outside `EncodeFrame` (skeleton lines, grid, particle billboards) keep
their own buffers: no ring frame is open there, `UploadTransient` would fall back to the
same fresh buffer.

Found on the way and fixed (separate commit): a scene of ParticleGraph emitters alone took
Metal's sky-only exit in `EncodeScene` (`onlySky` knew trails and skinned meshes only), so
it drew nothing.

### Measurements

`scripts/perf/bone_upload_probe.mm` (CPU side only, M5 Air, load ~7, three runs agreeing
within 1 %), 200 skinned objects per frame:

| Bones | Old per object | Ring per object | 200 objects/frame |
|---|---|---|---|
| 64 | 16.4 µs | 0.37 µs | 3.3 ms → 0.07 ms |
| 1 | 13.7 µs | 0.06 µs | 2.7 ms → 0.01 ms |

The audit estimated 0.2-1 ms per frame for 50-200 characters; the measured old cost is
higher. Not measured: an editor frame with many real characters (no such witness scene),
GPU-side effects, Intel/AMD Macs.

## Handoff

- **Topic / step:** 216, step 1 of 3
- **Branch / commit:** `claude/audit-optimierung-metal-backend-stufe-3` @ 410aeb24 (witness),
  d4c1754b (ring), 677fb3ad (sky-only exit), plus this doc and the probe
- **Done:** skinned bones, particle instances and trail VB/IB use the upload ring; particle-only
  scenes draw on Metal.
- **Files touched:** `MetalRenderer.mm:7127` `UploadBonePalette`; `:9396` `boneSlots`;
  `:13965` skinned; `:17848` particles; `:7171` trails; `:6452` `TPDraw::vbufOffset`;
  `:14119` `onlySky`; `MetalRenderer.h:205,1409` + comments 815-825, 1369-1395;
  `EditorApplication.cpp:9410` `HE_DUMP_PARTICLETEST`; `scripts/perf/frame_pixel_ab.sh` set `upload`.
- **Verified:** `frame_pixel_ab.sh <out> <tree> upload` on the build before (deploy copy) and
  after: skin_fwd/def, skinslot_fwd, rope_fwd/def, part_fwd/def all md5-identical, baseline
  noise floor 0 (two runs identical). Skinned, rope (deferred) and particle shots under
  `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`: no validation message, expected draw counts.
  Particle-only: draws 0 → 3 forward / 4 deferred, image shows the three clouds. Full
  `he_tests` (Release, from the build dir): 5431 cases, 3 red, all known on this Mac and outside
  this change: `test_hpak` scanContentDirectory (green with an empty HOME, 59/59) and two
  `test_profiler` order cases (green alone, 18/18); see Hive lesson 381 and
  `docs/audit-core-fundamente-2026-10-17.md`.
- **Not verified:** a mesh with joint ids above its pose's bone count (the witness has one
  joint; shader validation checks the whole 1 MB chunk, not the slice, so it cannot prove the
  slot sizing). That the three sites take the ring and not its fresh-buffer fallback rests
  on code position (`beginFrame` `.mm:16633` → `EncodeScene` → `endFrame`), not on
  instrumentation; the pixel A/B cannot tell the two apart. Intel/AMD Macs. Windows/Linux CI (Metal-only change; the witness is editor code).
- **Decisions taken:** GI tables stay for step 2 (see above). Trail buffers stay retained in
  `m_ribbonBuffers` (needed for the fallback buffer; on a ring chunk only a refcount).
  Joint ids ≥ 128 keep the full palette (as undefined as before).
- **Pending:** step 2 (REN-404/405/406), step 3 (REN-301).
- **Traps:** `frame_pixel_ab.sh <out> <tree> <set>`: the set is the THIRD argument, a set
  name in second place is taken as the tree and every shot comes back MISSING. A scene of
  only particles needs a build with 677fb3ad, else pair `PARTICLETEST` with `SECTIONTEST=1`.
- **Next steps:** 1. Step 2: in `EncodeGIAccelBuild` (`.mm` ~8400-8640) and
  `EncodeGISwAccelBuild` (~8287) keep instance/colour/land buffers persistent with a dirty
  flag; the colour/land-index uploads could use the ring only if every consumer runs in the
  same frame as the build. 2. Use the `upload` set plus `HE_DUMP_GI` shots (not bit-exact,
  measure a noise floor) for its A/B.
