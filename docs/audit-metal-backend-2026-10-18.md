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

## Step 2: REN-404/405/406, GI acceleration structures only rebuilt when the casters change

Before, every GI-active frame (GI or GI reflections on) retired and reallocated the TLAS,
its scratch and five tables, searched the unique-BLAS list linearly per caster, and every
mesh seen for the first time was built on a command buffer of its own with
`waitUntilCompleted`. Now (commit 8b421011):

- **Dirty check (REN-405).** `EncodeGIAccelBuild` still gathers the per-instance arrays
  each frame (`GIAccelSnapshot m_giAccelCur`: unique BLAS, mesh buffers, BLAS index,
  transform, two colours, land index) and compares them byte for byte with what the
  live TLAS was built from (`m_giAccelLast`). Equal: nothing is allocated, encoded or
  uploaded. Only transforms differ: a fresh TLAS, the colour/land/mesh-pointer/mesh-index
  tables stay. Anything else: fresh TLAS and tables, as before. `GIProbeSceneSignature`
  could not be reused: it ignores motion on purpose.
- **Lifetime.** The live snapshot holds a reference on each BLAS and vertex/index buffer
  it uses (`CFRetain`, handed to `RetireGIObject` when replaced). Without that, a mesh
  invalidated while the TLAS is cached would free the BLAS the TLAS points at, and a new
  BLAS could take its address and pass the compare. The invalidation loop also sets
  `m_giAccelDirty`. Every exit that builds nothing goes through `ReleaseGIAccel()`.
- **Upload ring.** The TLAS instance descriptors are read by the build alone, so they
  are a ring slice. The tables are read by the trace kernels in later frames and stay
  ordinary retained buffers.
- **REN-404.** BLAS → slot through `m_giBlasSlot` (cleared per frame, capacity kept).
  The per-object `giInstanceSurface` call stays: a Param-driven pin reads live slot
  values (slider drags), which no revision counter covers, and it is one lookup.
- **REN-406.** `BuildBLAS(mesh, encoder)` encodes onto one acceleration-structure encoder
  on the frame's command buffer, each build with its own scratch (builds inside one
  encoder may overlap). The TLAS encoder comes later in the same command buffer and
  declares every BLAS used, so Metal orders it after the builds. No CPU wait.
- **SW path.** Unchanged instances keep `m_giSwInstanceBuf`; an unchanged transform
  keeps its inverse. `BuildGILandscapeTable` keeps its buffer while the entries match.
- **Not done: refit.** A refit writes the live TLAS in place while the previous frame may
  still trace it, needs the refit usage flag, and needs a scratch that is not in flight.
  A moving scene builds a fresh TLAS per frame, as before, but without the table uploads.
  Persistent scratch was left out for the same reason.
- **Found and fixed:** `giReflRay` bound the colour buffer as the no-terrain placeholder
  for its `GILand` table. With fewer than six casters that buffer is shorter than one
  192-byte entry, and Metal API validation aborted every GI-reflection frame of such a
  scene (HW only; the SW placeholder could hit it too). It now binds a zeroed `GILandGpu`.

### Witnesses and measurements

`HE_DUMP_GIACCEL=1` logs one line per GI frame, saying whether the TLAS (SW: the instance
buffer) was rebuilt or kept. `HE_DUMP_GIMOVE=<m>` (with `GIBLEED=1|2`) moves the receiver
sphere for six frames before the ordinary settle frames. Shots: `MATERIALTEST=1 GIBLEED=1
FRAMES=8 TOD=0.35`, `AA=0`, `HE_SKY_TIME=30`, under `MTL_DEBUG_LAYER=1
MTL_SHADER_VALIDATION=1`, before = self-contained copy of the step-1 deploy.

- Static scene, HW, SW and HW reflections: `rebuilt (tables)` once, then `kept` in every
  later frame. Moving receiver: `rebuilt (transforms)` ×6 (HW) / `rebuilt` ×6 (SW), then
  `kept`. The receiver's shadow follows it in the GI-on image (ray traced, HW and SW), at
  the same place as the rasterized CSM shadow with GI off; a TLAS kept from the start
  position would leave it where it was.
- Pixels against the run-to-run noise (GI is not bit-reproducible, `frame_pixel_ab.sh:40`):
  before/after differ in 685-2161 pixels, none by more than 8 levels; two runs of the same
  build differ in 515-3175 pixels, none by more than 8. GI off: md5-identical before/after.
  HW reflections: before aborts in validation (the placeholder bug), after runs clean.
- `scripts/perf/gi_accel_probe.mm` (M5 Air on AC, no other load; grids of 2048 triangles,
  three frames in flight; two runs within 10 %):

| Case | Old | New |
|---|---|---|
| 100 new meshes in one frame (BLAS) | 103 ms render thread blocked | 8 ms encode, GPU done after 17 ms |
| 500 new meshes in one frame (BLAS) | 308 ms blocked | 42 ms encode, GPU done after 73 ms |
| 1k casters / 100 meshes, static, CPU per frame | 0.40 ms + TLAS build | 0.07 ms, no build |
| 1k casters, all moving, CPU per frame | 0.40 ms | 0.20-0.23 ms |
| 10k casters / 500 meshes, static, CPU per frame | 4.8 ms + TLAS build (GPU ~2 ms) | 0.70 ms, no build |
| 10k casters, all moving, CPU per frame | 4.8 ms | 1.36 ms |

The probe leaves out the per-object material lookup and the extractor walk (unchanged).
GPU time per TLAS build varied between modes (0.4-2.3 ms at 10k) without a pattern I can
explain; the claim is the build count (static: 0 per frame), not the per-build time.

## Step 3: REN-301, `MetalRenderer` as a Pimpl like D3D11/D3D12

The public `include/Backends/Metal/MetalRenderer.h` (1475 → 111 lines) holds only the
IRenderer overrides, `GetDevice`/`GetCommandQueue`/`GetFramePassDescriptor` (the editor's
ImGui bridge, `EditorApplication.cpp:1222`, `EditorUI.cpp:659`), `MetalOverlayContext` and
`std::unique_ptr<MetalRendererImpl> m_impl`. Everything else, every member, nested type,
constant and helper, is `class MetalRendererImpl` in `src/Backends/Metal/MetalRendererImpl.h`,
which only `MetalRenderer.mm` includes.

- **Methods moved with the state, not the other way round.** The 190 `MetalRenderer::`
  definitions in the .mm became `MetalRendererImpl::`, bodies unchanged. Moving only the data
  (D3D11's `p.member` style) would have rewritten ~3000 member tokens on 2182 lines of a file
  that topics 180/182/207/214 also edit before 0.7.0; this way 287 lines changed.
- **IRenderer's protected state** (`m_contentManager`, `m_world`, `m_editorCamera`,
  `m_renderPath`, `m_viewMode`, `m_overlayCallback`; 109 tokens) is read as `m_owner.m_x`.
  `m_owner` is the back-reference to the facade, which names the Impl its friend.
  IRenderer's nested types, `kWorldPreviewSlots`, `GetEnvironment` and `GetWaterRipples` are
  aliased on the Impl (`MetalRendererImpl.h:75-93`), so the bodies read as before.
- **Facade:** `MetalRenderer.mm:6650-6754`, one forwarder per entry point. The Impl is never
  subclassed, so the calls between entry points that used to go through the vtable now go
  straight to the same function.
- **Not done (the audit's later REN-301 parts, each a separate change):** the `MtlRef`
  handle for the ~160 `void*` + `CFBridgingRelease` pairs, per-feature substructs, the
  `LazyPipeline` helper for the eight `...Tried` flags. So the internal header is as big as
  the old one was; only what the includers see got small (not the audit's "85 %" for the
  backend as a whole).
- Line numbers in the step 1/2 handoffs: `MetalRenderer.h:<n>` (private part) is now
  `MetalRendererImpl.h:<n+18>`; `.mm` lines after 6650 moved by +104.

### Witnesses

- Build (Release): `RendererMetal`, `HorizonEditor`, `HorizonGame`, `he_tests` green, no new
  warnings (the 14 `MTLStorageModeManaged` deprecations were there before). EditorUI.cpp,
  EditorApplication.cpp and RendererFactory.cpp compile without the old header's transitive
  includes.
- Rebuild scope: a later edit to `MetalRendererImpl.h` recompiled `MetalRenderer.mm.o` only.
  Before, every header edit also recompiled RendererFactory, EditorUI and EditorApplication.
- Header cost for an includer (`-E` of a TU with just the include, RendererFactory's flags):
  105,952 → 97,872 lines, where `IRenderer.h` alone is 97,749. So the Metal header's own share
  went from 8,203 lines to 123. The `-fsyntax-only` time went 6.5 → 4.9 s, but that was
  measured while shots and tests ran, so it is indicative only.
- Pixels, before = self-contained copy of the HEAD deploy (no `MetalRendererImpl` symbol,
  `nm`; the new one has 306): `frame_pixel_ab.sh <out> <tree> all`, 50 shots (frame, shadow,
  upload sets, forward and deferred), noise floor 0 (two runs of the old build identical),
  old/new **all 50 md5-identical**. The Metal material preview (`HE_PREVIEW_DUMP`,
  `MATERIALTEST=displace PREVIEW=1`) is md5-identical over 2 old + 2 new runs. GI on
  (`MATERIALTEST=1 GIBLEED=1 FRAMES=8 TOD=0.35 GI=1`, forward and deferred): old/new differ
  in 9k-38k pixels by at most 2 levels, two runs of one build in 13k-35k by at most 2. That is
  inside the noise.
- `he_tests` full (from the build dir): 5431 cases, 3 red, the known ones (`test_hpak`
  scanContentDirectory green with an empty HOME, `test_profiler` green alone). test_culling
  (body checks on `MetalRendererImpl::EncodeFrame/ExtractFrame/EncodeShadowMap`),
  test_foliage_cluster, test_engine_materials, test_bloom_kernel, test_particle_graph,
  test_world_scale green.

## Handoff, step 1

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

## Handoff, step 2

- **Topic / step:** 216, step 2 of 3 (REN-404/405/406)
- **Branch / commit:** `claude/audit-optimierung-metal-backend-stufe-3` @ 8b421011 (code),
  789e5b5b (record + probe), plus this handoff
- **Done:** GI TLAS and tables kept while the caster set is unchanged, tables kept when
  only transforms move, instance descriptors from the ring, BLAS slot map, BLAS builds
  batched on the frame's command buffer without a wait, SW instance/inverse and landscape
  table reuse; `giReflRay` placeholder validation abort fixed.
- **Files:** `MetalRenderer.mm:8187` `BuildBLAS(mesh, encoder)`; `:8269` `giAccelLogEnabled`;
  `:8305` SW path; `:8415` `BuildGILandscapeTable`; `:8466` `ReleaseGIAccel`; `:8485`
  `EncodeGIAccelBuild` (compare at `:8610`); `:16035` `noLand` placeholder; `:16709`
  invalidation flag; `:16936` profiler scope `Metal::EncodeGIAccelBuild`; shutdown ~`:6830`.
  `MetalRenderer.h:1155-1205` (`GIAccelSnapshot`, comments rewritten);
  `EditorApplication.cpp:9140` `HE_DUMP_GIMOVE`; `scripts/perf/gi_accel_probe.mm`.
- **Verified:** build green (Release). Shots under Metal API + shader validation, before
  = step-1 deploy copy: static HW/SW/reflections build once then keep; GIMOVE rebuilds ×6
  then keeps, receiver shadow follows (GI on agrees with CSM, HW and SW); before/after
  within the run-to-run noise (no pixel > 8 levels); GI off md5-identical.
  `MOUNTAINTEST=1 GI=1 GIREFL=1 RENDERPATH=1`: 16 chunk BLAS built in one batched frame,
  then kept, validation clean (the chunks were out of view, so the image proves nothing;
  whether its land table had entries was not checked). Probe numbers
  above. Full `he_tests` (Release, from the build dir): 5431 cases, 3 red, the known ones
  (`test_hpak` scanContentDirectory green with an empty HOME 59/59, `test_profiler` green
  alone 18/18); he_tests does not link the Metal backend.
- **Not verified:** mesh invalidation (sculpt) while the TLAS is cached, other than by
  reading; a scene with landscapes (table reuse ran only as "no terrain"); an editor
  frame time with GI on a real project (no `he_perf_capture` run); the GPU-side cost of
  the batched BLAS frame; Intel/AMD Macs; macOS < 13 (no mesh-pointer table).
- **Decisions:** no refit and no persistent scratch (in-flight race, see above);
  material lookup per object stays (live Param slots); byte compares, a -0.0/NaN mismatch
  only costs a rebuild.
- **Traps:** `HE_DUMP_GIBLEED` only adds geometry inside `MATERIALTEST=1`; without it the
  scene is empty and the Shutdown summary line (printed only after a GI frame) is absent.
  `HE_DUMP_GIACCEL=1` gives the per-frame proof. `CAMX/CAMY` do not move the
  `MATERIALTEST` camera. Metal validation goes to the process's stderr, not the engine log.
- **Next steps:** 1. Step 3 (REN-301 Pimpl). 2. Optional: refit for moving scenes behind a
  completion-handler fence, measured with the probe's `moved` case.

## Handoff

- **Topic / step:** 216, step 3 of 3 (REN-301)
- **Branch / commit:** `claude/audit-optimierung-metal-backend-stufe-3` @ 289e3341 (code),
  plus this record
- **Done:** MetalRenderer is a Pimpl; state and helpers in `MetalRendererImpl`.
- **Files:** `include/Backends/Metal/MetalRenderer.h` (facade, 111 lines);
  `src/Backends/Metal/MetalRendererImpl.h` (new: old class body; aliases `:75-93`, `m_owner`
  `:183`); `MetalRenderer.mm:1` include, `:6650-6754` forwarders; `src/HE_Rendering/CMakeLists.txt`
  (source list); `tests/test_culling.cpp:3360,3415,3421-3422,3430,3440-3441` (grep strings).
- **Verified:** see "Witnesses" in step 3: build green, 50/50 pixel shots md5-identical with
  noise floor 0, preview md5-identical, GI within noise, full he_tests with only the 3 known reds.
- **Not verified:** multi-window (`AttachWindow`/`RenderWindow`), thumbnails, skeletal/particle
  previews, `RenderSceneImage` at runtime (they compile and only forward; no witness drove
  them). Metal validation layer not run for this step (no GPU-side change). Intel/AMD Macs.
  Windows/Linux CI (does not build Metal).
- **Decisions:** methods moved into the Impl instead of `p.member` rewrites (merge surface,
  see step 3). The Impl sits in its own internal header, not inside the 18k-line .mm. The
  public `UnlitViewActive`/`WireframeViewActive` moved to the Impl (no outside caller).
- **Traps / merge:** topic 214 (`claude/audit-optimierung-rendering-frontend-stufe-2`) adds
  `m_visibility`, `m_refineRuns` and an inline `VisibilityKey()` to the old header and changes
  both `ResolveMaterialShader*` signatures. After merging, those go into
  `MetalRendererImpl.h`. Its new test greps `void MetalRenderer::CullCameraObjects()`, which
  must become `MetalRendererImpl::` or the test fails at runtime. New members or helpers
  belong in `MetalRendererImpl.h`. New IRenderer state the backend reads goes through
  `m_owner`, and a new IRenderer nested type needs an alias there.
- **Next steps:** 1. Merge (214 first, then this topic). 2. Optional REN-301 follow-ups:
  `MtlRef` for the retained `void*`, per-feature substructs, `LazyPipeline`; each needs its own
  pixel A/B (`frame_pixel_ab.sh … all` stays bit-exact for a pure refactor).
