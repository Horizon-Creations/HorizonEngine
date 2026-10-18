# Audit-Optimierung: Rendering-Frontend (Stufe 2)

Thema 214, from `docs/audit/AUDIT.md` Stufe 2 and `docs/audit/modul_rendering.md`.

## Step 1: REN-101 / REN-1001 (material shader key per draw)

**Problem.** Every graph-material draw on every backend asked
`MaterialShaderLibrary::resolveShaders` for its `(hash, source)` pair. Each call copied
`customShaderFragGlsl` and `customShaderVertGlsl` into fresh strings and ran
`std::hash<std::string>` over both, only to find the pipeline cache hit it nearly always
hits. REN-101 (D3D11), REN-201 (D3D12), REN-602 (GL), REN-801 (Vulkan) and the Metal
call site are all this one cause.

**Fix.**

- **Memo on the asset.** `MaterialAsset::shaderKeyMemo` (`Assets.h`, after
  `customShaderGBufGlsl`) holds `std::hash` of the fragment, vertex and G-buffer sources.
  It is not serialized. Copying or moving the asset gives an empty memo, so a memo only
  ever describes the object it sits in: `replaceMaterial`, hot reload and slot reuse all
  start clean.
- **Invalidation.** `ContentManager::getMaterialMutable` bumps
  `MaterialAsset::shaderRevision`. The memo is trusted only while the revision is
  unchanged and each string's `data()` and `size()` are unchanged. Every write site in
  the tree was checked: each one either fills a local `MaterialAsset` before
  `registerMaterial`, or uses a pointer it just took from `getMaterialMutable`.
  - The one change the memo cannot see is a same-length in-place edit through a mutable
    pointer kept across a render. The rule is written next to the fields.
  - `PropertyAnimationSystem` calls `getMaterialMutable` every frame for animated
    materials. Those materials rehash once per frame, which is the old cost, for them
    only.
- **Pointer form.** `resolveFragment`, `resolveShaders` and `resolveGBufferShaders` have
  `const std::string*&` overloads that return the asset's own strings without a copy.
  The `std::string&` overloads are now thin wrappers that copy and no longer hash.
- **Hash.** The key formula is unchanged, bit for bit: `frag`, then
  `^= vert * 0x9E3779B97F4A7C15` when there is a WPO body.
- **Backends.** The per-draw sites now use the pointer form:
  - GL: `resolveMaterialShader` and `resolveMaterialShaderGB` (all 10 callers).
  - Metal: `ResolveMaterialShader` and `ResolveMaterialShaderGB` (all 9 callers).
  - Vulkan: `VulkanRenderer.cpp:8253`.
  - D3D11: `D3D11Renderer.cpp:7009`.
  - D3D12: `D3D12Renderer.cpp:10974`.

  The pre-warm loops (`Vulkan:7669`, `D3D11:5319`, `D3D12:4407`) stay on the copying
  overload. They run once, not per frame.

**Measurement** (`tests/test_material_shader_resolve.cpp`, third case). The fixture is
the auto-landscape material, 22 260 bytes of fragment plus vertex. The test times
1000 resolves per frame over 31 frames and reports min/median. Release build, M5,
load average 8–11 (other workers were building):

Three runs each; the cells are the medians of the three runs, with the per-run minimum
in brackets.

| Path, 1000 resolves | unchanged code | with the fix |
|---|---|---|
| copy + hash both strings (legacy loop in the test) | 9 947 / 10 370 / 10 508 (min 4 490 / 5 097 / 7 793) | 6 379 / 6 386 / 6 401 (min 2 626 / 2 647 / 3 364) |
| `resolveShaders(std::string&)` | 10 023 / 10 821 / 9 686 (copy + hash) | 1 853 / 1 657 / 1 657 (copy only) |
| `resolveShaders(const std::string*&)` | — | **80.5 / 81.6 / 81.6** |

All values are µs per frame. Before the fix the string overload *was* the old path,
and it matches the legacy loop. The legacy loop also differs between the two builds
(about 10 ms vs 6.4 ms) with the same code, which shows how much the load moved the
numbers. Comparing within one run: the pointer form costs about 0.08 µs per draw against
about 6.4 µs for copy + hash, roughly 80 times less. The pointer row was stable across
all three runs. This is a micro benchmark, not a scene capture: the real share
of the frame depends on how many graph-material draws a scene has.

## Step 2: REN-901 / REN-902 / REN-1002 (DrawCall deep copies)

**Problem.** `DrawCall::instanceTransforms` and `SkinnedDrawCall::boneMatrices` were
`std::vector<glm::mat4>` owned by each draw. GeometryPass filled the list, every slot of a
multi-section mesh copied it (`DrawCall sd = d`), and `recordDraw(const&)` copied it again.
A run above `kMaxInstancesPerDraw` also built a temporary `all` and `assign`ed each chunk.
At 140k plants with 3 slots, every matrix was copied about four times per frame. `reset()`
also freed every per-draw vector, so each frame allocated again.

**Fix.**

- **`HE::Mat4Range`** (`CommandBuffer.h:25`) is a read-only `[first, count)` window onto
  a flat `std::vector<glm::mat4>` the CommandBuffer owns. It stores a pointer to the
  vector object, not to its data, and resolves `data()` when read, so the arena may
  reallocate while later draws are still being recorded. It reads like the vector it
  replaced (`empty/size/data/[]/begin/end`), so the GL, Metal and Vulkan loops did not
  change. D3D11 (`:2570`, `:3590`) and D3D12 (`:6651`, `:7861`) retype one
  `const std::vector<glm::mat4>& inst` alias each.
- **GeometryPass** (`RenderPass.cpp:83-205`): a run is unfolded once with
  `appendInstances` straight into `matrixArena()`. Slots share the window, and split chunks
  are `slice`s of it. The skinned pose goes in once via `storeMatrices` and the slots
  share it. Every record call uses the new `&&` overloads.
- **CommandBuffer:** `reset()` keeps the arena's capacity. Copy and move rebase the
  windows onto the new buffer's arena. `recordDraw(const&)` of a draw whose window names
  another buffer copies the matrices into this buffer's arena. `storeMatrices` copes with
  a source inside its own arena.
- **Unchanged contracts:** draw order, "one instance = empty list, matrix in
  `transform`", and the name `instanceTransforms`. The latter matters because the source
  guard in `test_foliage_cluster.cpp:1098` greps for it.

**Measurement** (`test_rendergraph.cpp:1079`, `HE_GEOMPASS_BENCH=1`). Median of 31
frames into one reused CommandBuffer. Before and after ran alternately, three times each,
same machine and load. No `yes` hogs, low-power mode off, load average 3–8.

| Scene | before | after |
|---|---|---|
| one run, 140k instances x 3 slots (split path, 9 draws) | 20.2 / 22.5 / 22.7 ms | 17.3 / 17.4 / 17.4 ms |
| 200 runs x 250 instances x 3 slots (600 draws) | 6.6 / 6.7 / 6.8 ms | 4.65 / 4.8 / 4.8 ms |
| 500 skinned x 64 bones x 2 slots (1000 draws) | 1.23 / 1.25 / 1.26 ms | 0.385 ms (all three) |

The matrix copy is now ~9 MB once, which is about 1 ms at memory speed. The remaining
~16 ms of the split run were not profiled. The likely cost is the run-extension loop
(`RenderPass.cpp:31-61`), which compares `sections` (a vector) and five other fields per
object of a 140k-object run. That is an estimate. Clustered foliage sends one object per
bucket and does not pay it.

**Pixel A/B** (`/tmp` witness script on top of `he_shot.py`: `HE_SKY_TIME=30`, `AA=0`,
private config). The before deploy was a self-contained copy built from `HEAD~1`'s five
source files. 18 shots, Metal and OpenGL each: foliage 2000 and 100 000 fwd/def, the
instanced-cube shadow row, `SECTIONTEST=1` fwd/def, `=skinned`, `=skinnedslot`, and the
occlusion batch. All are **md5-identical** before vs after, and the dump counters are
identical. Two runs of the before build were identical too (noise floor 0). The images
were looked at: the foliage field and the 3-slot skinned sphere are there.

## Step 3: REN-805 / REN-304 / REN-407 (cull + sort once per walk)

**Problem.** `RenderExtractor::FrameScope` already leaves one walk of the world in the
backend's RenderWorld for the whole frame, but every pass culled and sorted it again.
Vulkan did it four times against the camera (decal depth, GI, SSAO, scene), Metal up to
four times (`CullCameraObjects`: GI G-buffer, SSAO pre-pass, scene, deferred G-buffer). All
five backends also culled and fully sorted once per shadow layer (up to 3 cascades and
16 local layers), with a sort key (mesh, distance to the *camera*) that is the same for
every layer. On top of that, Metal's shadow pass wrote into the camera's
`m_visible`/`m_sortedIndices`.

**Fix.** `HE::FrameVisibility` (`FrameVisibility.h:23`, `.cpp`) is one instance per
backend, next to its extractor.

- **Camera** (`cullAndSortCamera` `:45`; `findCamera`/`storeCamera` for Metal, which has
  the occlusion refine between cull and sort). The result of the walk is kept, keyed on
  `RenderExtractor::fullExtractCount`, a refine stamp (`m_refineRuns`, bumped whenever
  `refineObjectBounds` actually loops), a variant (Metal: occlusion on/off), the object
  array and count, the view-projection (compared exactly, so the SSAO pass's other
  aspect misses), and the camera position. A hit copies the result into the pass's
  vectors, so a pass that reused them in between still gets the right answer. Vulkan
  `:3891/7838/11507/12652`, Metal `:17950/17985`.
- **Shadow layers** (`shadowLayer` `:65`). The first layer of a walk culls every view
  that `world.shadow` lists (cascades, local layers) and keeps each cull. It sorts the
  casters of the **union** once (`RenderSorter::sortShadowCasters`, `RenderSorter.cpp:44`,
  index as the last tie-break). Every layer is then an O(n) filter of that list
  (`keepVisible` `:76`) followed by `batchDepthCasters`. A view that is not in the list
  takes the old cull + sort and bumps a counter. The views are part of the key, because
  the extractor's aspect tail refits the cascades in place without a new walk. All five
  backends call it: GL `:11096`, D3D11 `:6655`, D3D12 `:10653`, Vulkan `:7007`, Metal
  `:8009`.
- The first design sorted *every* caster once per walk. The bench showed that this loses
  in a world larger than the shadow range (a few percent of the objects in any view),
  so the presort now covers the union only. It is never more than the layers sorted
  between them before.

**Equivalence.** Same runs with the same members as the per-layer path. With ties
(equal mesh and distance) the order inside a run can differ, but a depth-only run has
no order-dependent output. The camera order is unchanged (same `RenderSorter::sort`).
Tests in `test_culling.cpp:4167-4378` cover this: per-view order without ties; same
batches incl. ties, skip entity, a foreign view, a refit cascade, a moved camera, an
added object and a refined bound; camera reuse and every key miss; the refine stamp; and
a source pin on every backend.

**Measurement 1, CPU bench** (`test_culling.cpp:4379`, `HE_FRAMEVIS_BENCH=1`). 50k
objects, 200 meshes, 11 layers (3 cascades + 8 local), median of 31, two runs each,
load average 3-5 (other bees on the device), low-power mode off:

| World | shadow before | shadow after | camera x4 before | after |
|---|---|---|---|---|
| dense, 448 m (union 43.8k) | 22.2 / 22.5 ms | 17.3 / 17.3 ms | 14.0 / 14.2 ms | 3.5 / 3.5 ms |
| open, 2016 m (union 2.2k) | 6.5 / 6.7 ms | 5.24 / 5.27 ms | 3.3 / 3.4 ms | 0.81 / 0.85 ms |

New finding: **one `std::sort` of 50k keys costs 9.2-9.5 ms**, against 0.75-1.1 ms to
build the keys. The time is in the three-field comparator (about 12 ns per comparison),
not in memory. That single sort is now the largest remaining shadow cost in a dense
world. A union OR loop over `std::vector<uint8_t>` cost 1.5 ms for 11 layers until it
used raw pointers (a `uint8_t` store may alias the vector itself).

**Measurement 2, real frame on Metal.** `he_perf_capture.py`, 300 frames, before deploy
= self-contained copy of `3f1fbc0b`. Scenes from `gen_reference_world.py --count 50000
--shadow-lights 2` (new option: 2 shadow-casting point lights, so 3 cascades + 12 local
layers), camera `0,25,90,0,-0.25`. The screen was **locked** and another process held
the GPU at about 63%, so treat the absolute GPU figures as soft. Before and after ran
back to back under the same conditions.

| p50, ms | dense before | dense after | open before | open after |
|---|---|---|---|---|
| frame CPU | 77.4 | 69.2 | 54.4 | 49.8 |
| `Metal::EncodeShadowMap` (CPU) | 31.9 | 27.7 | 12.2 | 10.4 |
| of which `FrameVisibility::shadowLayer` | - | 25.6 (first layer 18.0) | - | 9.6 (first 8.8) |
| `Metal::EncodeScene` (CPU) | 7.7 | 3.2 | 5.0 | 2.2 |
| `FrustumCuller::cull` calls / frame | 17 | 16 | 17 | 16 |
| GPU `Shadow` pass | 12.9 | 11.8 | 4.0 | 3.5 |

**Cull + sort per layer vs the GPU shadow pass:** the CPU cost of the shadow layers is
2-3x the GPU time of the whole shadow pass (25.6 vs 11.8 ms dense, 9.6 vs 3.5 ms open).
A GPU-side cache of static layers, which would skip their depth draw, would therefore
leave the bigger half in place. The CPU-side "static" part is in: one sort per walk. Not
built: caching layers *across* frames. That needs a dirty signal for "no caster moved"
(Stufe 4 dirty gates), and the camera position is part of the sort key.

**Pixel A/B.** `scripts/perf/frame_pixel_ab.sh <out> <tree> all` (Metal): 42 shots
(cascades, local point and spot, day-night, decals, foliage, odd render scale). All 42
are md5-identical before vs after, and two after runs were identical (noise floor 0).
All 42 md5s differ from each other. `ls_point_day_fwd` and `shadinst_fwd` were looked at;
the cascade and local shadows are in the picture. OpenGL (same env, `RHI=OpenGL`):
`ls_point_day_fwd`, `ls_spot_fwd`, `shadinst_fwd` are md5-identical before vs after, and
their md5s differ from the Metal shots. The point light frame was looked at.

## Step 4: REN-903 / REN-908 / REN-401 (cluster build, SSAO kernel, sky IBL bake; M8)

**Cluster build (REN-903).** `BuildClusterLights` allocated 3456 `std::vector<uint32_t>`
per call (one per cell), filled them by `push_back` and copied them flat. It is now a
two-pass counting sort (`LightPacking.cpp:141`): pass 1 admits the lights exactly as
before and only counts per cell, keeping each light's cell box (`CellBox` `:164`, stack,
256 entries); the prefix sum gives the offsets; pass 2 (`:263`) writes the indices in
light order, so a cell's list is still ascending by light index. Count and cursor tables
are on the stack (~15 KB). New in-place overload `BuildClusterLights(out, ...)`
(`LightPacking.h:222`) plus `ClusterLightBuild::clear()` (`:199`, keeps capacity); the
by-value form calls it. GL `:11448`, Vulkan `:7861`, D3D11 `:6323` and D3D12 `:10308`
keep the build as a member scratch. Metal is unchanged (by value, two builds per frame
in deferred, see below). `camFwd` still comes from `glm::inverse(view)`: the view-row
shortcut is the same direction but not the same floats, and it feeds the shaders.

**SSAO kernel (M8).** The generator was already shared, but Metal (`:12593`) and D3D11
(`:2628`) rebuilt the kernel every frame they ran SSAO, and four backends each had their
own vec3 to vec4 loop. `HE::SsaoKernelVec4()` (`SsaoKernel.h:45`) builds it once per
process (function-local static), padded. Metal, D3D11, D3D12 (`:5797`) and Vulkan
(`:12162`, the double memcpy is gone) copy it. GL keeps its one-time `glUniform3fv`.

**Sky IBL bake (REN-908 / REN-401).** `HE::SkyEnvBaker` (`SkyEnvBake.h:260`,
`src/SkyEnvBake.cpp`, new, also in `tests/CMakeLists.txt`) replaces the five copied
drivers (fresh 1.5 MiB vector, `parallel_for`, serial half conversion behind each
backend's own dead band). It owns the face size (`kSkyEnvFaceSize`, the four `128`s),
the last baked sun, a reused scratch, and the bake. `bakeHalf` packs halves inside the
row task and can write straight into mapped staging (Vulkan `:3028`, D3D12 `:8977`).
Backends: GL `:4203`, Metal `:13862`, Vulkan `:3023`, D3D11 `:4621`, D3D12 `:8972`. Each
keeps its own `m_skyEnvValid` (the texture holds a bake), so a recreated cube bakes at once.

**Measurement** (`test_culling.cpp:1090`, `HE_SKYENV_BENCH=1`; M5, 10 pool threads, no
`yes` hogs, low-power mode off, load average about 2.4):

- **One 128² bake costs 52-59 ms with the pool and 284-298 ms serially.** It blocks the
  render thread. The audit estimated 1-10 ms and the old code comments said "a few ms".
  Under day-night auto-advance Metal (1e-4 dead band) baked on every frame, so the frame
  could not be shorter than the bake (~17 fps). GL/Vulkan/D3D baked every third frame at
  60 fps, which is 20 stalls of ~55 ms a second, so in practice the frame rate collapses
  there as well.
- Packing halves in the row task saves nothing measurable (52.3 vs 53.7 ms); the bake
  dominates.
- Where the cube changes (largest per-frame change of the cube's mean colour, 120 s day,
  60 fps, per 1/24 of the day): about 1-2 % a frame around sunrise and sunset, 0.03-0.05 %
  in full day, 0.00 % at night. More than 0.2 % a frame only for sun y in [-0.10, 0.16].

So the dead band follows the sun's elevation (`SkyEnvSunTolerance`, `SkyEnvBake.cpp`):
GL's 2e-3 (0.11°) when either sun is in the twilight band y in [-0.36, 0.27] (wider than
the measured [-0.10, 0.16] on purpose); 0.5° when both are in full day (under one 128²
texel of 0.7°, so the sun disk baked into the cube trails by less than a texel); 2° when
both are deep in the night. There is no clock, so it is frame-rate independent, and a
headless frame gets exactly the cube of its sun's last bake. A first try with a flat
1/15 s interval was dropped: it cost up to 26 % cube error on a 20 s day, in twilight
where the error shows.

Simulated per policy over one full day at 60 fps (the extractor's sun path, the shipped
`due()`). "Error" is the largest relative gap, over all frames, between the mean colour of
the cube in use and that of a cube baked for that frame's sun (16² truth table):

| Day length | Policy | Bakes / s | Worst error | Sun trails by |
|---|---|---|---|---|
| 120 s | Metal before (1e-4) | 60 | 0 % | 0.03° |
| 120 s | GL/Vk/D3D before (2e-3) | 20 | 3.37 % | 0.09° |
| 120 s | shared baker | **7.2** | 3.47 % | 1.96° (night band), 0.5° by day |
| 20 s | before (all five) | 60 | 0 % | 0.03° |
| 20 s | shared baker | **28.5** | 0.41 % | 1.92° (night band) |

The worst error at 120 s is in twilight, where the rule is the old 2e-3. It differs from
GL-before only in where the bakes fall. This is a simulation at a fixed 60 fps, not a
real frame: the dump mode has no auto-advance and no project was at hand for
`he_perf_capture.py`. Twilight is still bound by the 55 ms stall about 24 times a
second. Throttling cannot fix that (see the handoff).

**Cluster bench** (`test_culling.cpp:1947`, `HE_CLUSTER_BENCH=1`, median of 31; "before"
is the old builder kept verbatim in the test as the reference):

| Lights (admitted / indices) | before | by value | in place |
|---|---|---|---|
| 8 (6 / 83) | 58 µs | 13.5 µs | 11.5 µs |
| 50 (40 / 8245) | 523 µs | 41 µs | 37 µs |
| 256 (195 / 27618) | 1038 µs | 145 µs | 140 µs |

**Equivalence.** The counting sort matches the old builder element for element in 7
seeded light sets x 8 flag combinations (no light, over 256 lights, index-cap overflow,
lights at and behind the camera, directional lights in between, both uv origins), through
one scratch reused from big to small (`test_culling.cpp:1900`). The baker's floats equal
`BuildSkyEnvFace` and its halves equal `packHalf1x16` of them (`:990`). The face hash
pin is untouched. A source pin (`:4969`) keeps every backend on the baker and the packed
kernel.

**Pixel A/B.** The before side is a separate worktree at `493bae57` with its own Release
build. Its deploy has no `SkyEnvBaker` symbol; the after side has 4.
`scripts/perf/frame_pixel_ab.sh <out> <tree> all` (Metal, 42 shots: local point/spot
lights forward and deferred-clustered, SSAO at render scale 0.51, eight day-night sky
times, cascades, decals, foliage): all 42 md5-identical before vs after, two after runs
identical (noise floor 0), 42 distinct md5s. Looked at `ls_point_def`, `dn_25_fwd` and
`odd_shadinst_fwd`; each shows its subject. OpenGL (`he_shot.py ... RHI=OpenGL`):
the day point light, the twilight `dn_25` and the SSAO odd scale are md5-identical
before vs after, and differ from the Metal shots. A static sun bakes on the first frame
under every policy, so no witness can show the dead bands. Only a moving sun can, and
the bench above is that evidence.

## Handoff (step 1)

- **Topic / step:** 214, step 1 of 4
- **Branch / commit:** `claude/audit-optimierung-rendering-frontend-stufe-2` (see the hive post for the SHA; pushed)
- **Done:** The graph-material shader key is memoised on the asset. The draw loops of
  all five backends resolve it without a copy or a hash.
- **Files touched:**
  - `src/HE_Core/include/ContentManager/Assets.h`: `shaderRevision` and `ShaderKeyMemo`, with the contract.
  - `src/HE_Core/src/ContentManager/ContentManager.cpp`: `getMaterialMutable` bumps the revision.
  - `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp`: `shaderKeys()`, `foldVertex()` and the pointer overloads.
  - Backends: GL and Metal wrappers and callers; Vulkan, D3D11 and D3D12 per-draw sites.
  - `tests/test_material_shader_resolve.cpp` (new, listed in `tests/CMakeLists.txt`).
- **Verified:**
  - `he_tests` built (Release, `out/build/release`). The new 3 cases pass (bit identity
    against the reference formula on the first and the repeat call, invalidation, timing).
  - `Material instance resolves to the master's shader hash…` (`test_material_graph`) passes.
  - Full `ctest -j4` (Release): 303/304, 2 skipped. The one failure is the known
    `test_hpak` "Disk registry" (6 == 2; the scan sees the user's engine-content cache
    under `HOME`, see `docs/audit-core-fundamente-2026-10-17.md`). With an empty `HOME`
    it passes 59/59, 766 assertions.
  - Metal and GL objects compile.
  - `VulkanRenderer.cpp` passes `clang -fsyntax-only` against MoltenVK headers, with a
    negative control: the old non-dereferenced call fails at the edited line.
- **Not verified:**
  - D3D11/D3D12 are compile-unverified locally (no Windows toolchain on the Mac); the CI
    run decides.
  - No GPU run of any backend: no frame was rendered with the change.
  - The memo is not thread-safe. It is written under a `const` accessor and assumes
    resolves happen on the render/main thread, as all current callers do (the draw loops
    already mutate non-thread-safe pipeline caches right after the resolve).
- **Decisions taken:**
  - The memo lives on the asset with copy-reset semantics, not in a map in the library.
    With copy-reset it can never describe another object, and unloads need no hook.
  - The revision bump is in `getMaterialMutable`, not at the ~40 string write sites.
  - The audit's point 3 (per-frame backend map per `materialAssetId`) was not built.
    After the memo a resolve is two lookups.
- **Decisions suggested, not taken:**
  - `OpenGLRenderer.cpp` ~11840 calls `matGlslTextureArrayMask(ma->customShaderFragGlsl)`
    per draw, which scans the whole fragment again. It belongs to the same class of
    problem, but is a different finding (REN-602 neighbourhood). It could be memoised in
    the same `ShaderKeyMemo`.
  - The UI quad paths (`GetOrBuildUIMaterialProgram` in GL and Metal) still run
    `fragGlsl->find("heBackdrop")` per quad (REN-403/604).
- **Traps:**
  - `ContentManager` and `MaterialAsset` live in the global namespace, not in `HE`.
  - Overwriting a slot with `*getMaterialMutable(id) = other` also overwrites the
    asset's `id`, and the lookup then fails. Use `replaceMaterial`.
  - A `.mm` or GL edit is caught early with `ninja <obj>.o` before the full `he_tests` build.
- **Next steps:**
  1. Step 2 (REN-901/902/1002, DrawCall moves): start at `CommandBuffer::recordDraw` and
     `RenderPass.cpp:78/102/147-168`.
  2. If the Windows CI job fails in `D3D11Renderer.cpp:7009-7042` or
     `D3D12Renderer.cpp:10974-11008`, the cause is the `*matFrag` / `*gbFrag`
     dereference there (same shape as Vulkan).

## Handoff (step 2)

- **Topic / step:** 214, step 2 of 4 (REN-901/902/1002)
- **Branch / commit:** `claude/audit-optimierung-rendering-frontend-stufe-2`, code
  `1c723556` plus this doc commit, pushed.
- **Done:** instance and bone matrices are windows (`HE::Mat4Range`) into one matrix
  arena per CommandBuffer. Slots and split chunks share one copy, record calls move,
  and `reset()` keeps capacity.
- **Files touched:** `CommandBuffer.h` (Mat4Range `:25`, fields `:99`/`:116`, API
  `:124-165`), `CommandBuffer.cpp`, `RenderPass.cpp:83-205`, the D3D11/D3D12 alias lines
  (above), `tests/test_rendergraph.cpp` (7 cases `:931-1077`, bench `:1079`), and
  `tests/test_skeletalmeshcomponent.cpp:50/65` (bones via `cb.storeMatrices`).
- **Verified:**
  - he_tests: rendergraph 39/39, foliage_cluster 20/20, skeletalmesh 9/9, weather and
    culling green.
  - Full `ctest -j6` without `test_material_graph`: 302/303. The one failure is the known
    `test_hpak` "Disk registry" 6 == 2 (4 cache files). It passes with an empty `HOME`.
  - HorizonEditor builds; the GL and Metal objects were rebuilt after the header change.
  - Vulkan `clang -fsyntax-only` against MoltenVK passes; the negative control without
    the include fails at `vulkan/vulkan.h`.
  - The Metal and OpenGL pixel A/B is md5-identical (see above).
- **Not verified:** D3D11 and D3D12 do not compile on the Mac. CI decides; the change
  there is the four alias retypes only. `test_material_graph` was not run (slow; it does
  not touch DrawCall).
- **Decisions taken:**
  - Use a window with an arena pointer plus offset instead of the audit's
    `{instanceFirst, count}` + `cmds.instances()`. The backend loops (about 70 sites)
    stay untouched, and the window still resolves at read time.
  - Bones share the same arena.
  - `paramOverride` stays a `std::vector<float>`. The backends take it as
    `const std::vector<float>*` or copy it into their own `params`; override objects are
    never batched.
- **Not done (left for later):** REN-1003 (RenderExtractor copies
  `smc.boneMatrices` → `obj.boneMatrices`, `RenderExtractor.cpp:639`), the `paramOverride`
  copy per run/slot, and the per-object `sections` compare in the run-extension loop.
- **Traps:**
  - A DrawCall's windows are valid only until its CommandBuffer is `reset()` or destroyed.
    Keep no DrawCall across frames (no backend does today).
  - To set matrices on a hand-built DrawCall, go through `cb.storeMatrices(...)`. A
    `= { ... }` initializer no longer compiles.
  - Only ever append to `matrixArena()`.
- **Next steps:**
  1. Step 3 (REN-805/304/407): Cull and sort once per frame.
  2. If Windows CI fails at D3D11 `:2570`/`:3590` or D3D12 `:6651`/`:7861`, look at
     what the `inst` alias is used for below those lines. Only `size/[]/empty/range-for`
     were found.

## Handoff (step 3)

- **Topic / step:** 214, step 3 of 4 (REN-805/304/407)
- **Branch / commit:** `claude/audit-optimierung-rendering-frontend-stufe-2`, code
  `1ea4e67f` plus this doc commit, pushed.
- **Done:** the camera cull + sort runs once per walk on Vulkan (4 passes) and Metal
  (`CullCameraObjects`). Every backend's shadow layers share one caster sort per walk
  (the union of the layer culls) and filter it per layer. Measured in the bench and in
  a real Metal frame (step 3 above).
- **Files touched:** `HorizonRendering/FrameVisibility.h` + `src/FrameVisibility.cpp`
  (new, listed in `src/HE_Rendering/CMakeLists.txt` and `tests/CMakeLists.txt`),
  `RenderSorter.h/.cpp` (`sortShadowCasters`, `keepVisible`), the backend sites listed
  above, `MetalRenderer.h`/`VulkanRenderer.h` (`m_visibility`, `m_refineRuns`,
  `VisibilityKey()`), `tests/test_culling.cpp:4100-4500`, and
  `scripts/perf/gen_reference_world.py` (`--shadow-lights`).
- **Verified:** `test_culling` 72/72 cases (16991 assertions); full ctest `-j4` without `test_material_graph`: 302/303, 2 skipped. The one failure is
  the known `test_hpak` "Disk registry" 6 == 2 (4 cache files); it is 59/59 with an empty
  `HOME`.
  HorizonEditor builds (Metal + GL). Vulkan `clang -fsyntax-only` against MoltenVK: rc 0;
  the negative control without MoltenVK fails at `vulkan/vulkan.h`, and a typo at
  `cullAndSortCamera` fails at the three sites. Pixel A/B: Metal 42/42 and OpenGL 3/3
  md5-identical. Bench and real-frame figures above. Editor, game and scene code do
  no cull or sort of their own (grep), so the source pin covers every caller.
- **Not verified:** D3D11 and D3D12 do not compile on the Mac; CI run 39080059803
  decides. The change there is one call plus one member each, the same shape as GL.
  Vulkan was not rendered (no device). `test_material_graph` was not run.
- **Decisions taken:**
  - The owner is a new frontend class, not `RenderGraph`. RenderGraph is a pass list
    that only D3D11/D3D12 drive, and it does not see the Metal/Vulkan/GL pass functions.
  - The presort covers the union of `world.shadow`'s views, not every caster (the
    open-world regression, see above). A view outside the list falls back.
  - GL, D3D11 and D3D12 keep their one camera cull (no repeat to save there). Vulkan's
    `m_culler`/`m_sorter` are gone.
  - No GPU skip of static shadow layers: the CPU side measured 2-3x larger.
- **Decisions suggested, not taken:**
  - Make `RenderSorter::sort` itself cheaper (a packed 64-bit key per object, mesh rank
    plus float bits, then a radix or plain integer sort). The 9.4 ms per 50k is
    comparator-bound. Equal keys would come out by index, which changes camera draw
    order on exact ties only, so it needs its own pixel A/B.
  - Cull all of a walk's shadow views in one pass over the objects (one loop testing
    every frustum) instead of one `parallel_for` per view: the culls are now the largest
    part in an open world (15 x 0.46 ms).
- **Traps:**
  - `FrameVisibility` caches by key only. A pass that changes `worldBounds` after a cull
    without going through `refineObjectBounds` (which bumps `m_refineRuns`) would get a
    stale camera answer. The same holds for the shadow-layer culls of a walk.
  - `std::vector<uint8_t>` indexed in a hot loop that also stores bytes: use raw pointers
    (aliasing kills vectorisation, 1.5 ms vs ~0).
  - The real-frame capture with a locked screen: GPU figures are soft
    (`CGSSessionScreenIsLocked`, see the memory note `perf-measurement-on-m5`).
- **Next steps:**
  1. Step 4 (REN-903/908/401): cluster build without heap, SSAO kernel built once, a
     shared and throttled sky IBL bake.
  2. If the Windows CI job fails at D3D11 `:6655` or D3D12 `:10653`, check that
     `HE::FrameVisibility` is reachable there (include at the top, member in the Impl
     struct at D3D11 `:1485` / D3D12 `:1529`).
  3. Later, its own step: the faster sort and the single-pass multi-frustum cull from
     "suggested" above.

## Handoff

- **Topic / step:** 214, step 4 of 4 (REN-903/908/401, M8)
- **Branch / commit:** `claude/audit-optimierung-rendering-frontend-stufe-2`, code
  `cdfc33a7` + `30197ab8` plus this doc commit, pushed.
- **Done:** counting-sort cluster build with an in-place overload (GL/Vulkan/D3D11/D3D12
  keep a member scratch); the SSAO kernel built once (`SsaoKernelVec4`); one shared sky
  IBL rebake driver (`HE::SkyEnvBaker`) in all five backends, with dead bands by sun
  elevation. Measured, see step 4 above.
- **Files touched:** `LightPacking.h/.cpp`, `SsaoKernel.h/.cpp`, `SkyEnvBake.h` +
  `src/SkyEnvBake.cpp` (new; `src/HE_Rendering/CMakeLists.txt`, `tests/CMakeLists.txt`),
  the five backends (sites above), `OpenGLRenderer.h`, `VulkanRenderer.h`,
  `MetalRenderer.h` (one include `:18`, one member `:642`), `tests/test_culling.cpp`.
- **Verified:** `test_culling` 78/78 cases (17906 assertions). Full `ctest -j6` without
  `test_material_graph`: 300/301, 2 skipped; the one failure is the known `test_hpak`
  "Disk registry" 6 == 2 (4 cache files), green with an empty `HOME`. HorizonEditor
  builds. Vulkan `clang -fsyntax-only` against MoltenVK: rc 0, and the negative control
  fails at `vulkan/vulkan.h`. Pixel A/B: Metal 42/42 and OpenGL 3/3 md5-identical.
- **Not verified:** D3D11/D3D12 do not compile on the Mac; CI decides (members, one
  call each, the kernel copy, the baker calls; the `glm/gtc/packing.hpp` includes
  there were removed as unused). No real frame under auto-advance: the rebake rates
  are a simulation (bench), the bake cost is measured. Vulkan not rendered.
  `test_material_graph` not run.
- **Decisions taken:**
  - Dead bands by sun elevation, no clock. A flat time interval was tried first and
    measured worse in twilight (26 % on a 20 s day).
  - Metal's 1e-4 is gone; it now rebakes like GL/Vulkan/D3D in twilight.
  - `camFwd` keeps `glm::inverse(view)` (bit identity of the shader input).
  - Face size stays 128² (no image change).
  - The bake stays synchronous. Async touches the five upload paths (Vulkan/D3D12
    per-frame staging slots) and two of them can't be built here.
  - Metal keeps building its clusters by value (`MetalRenderer.mm` is held by topic 216).
- **Decisions suggested, not taken:**
  - **The bake itself is the real cost: 52-59 ms per 128² bake on 10 threads, on the
    render thread**, about 24 times a second through twilight. Options, with numbers:
    64² faces (4x cheaper, changes ambient on all platforms, re-pin the face hash);
    an async bake on a dedicated thread with a double buffer (not a pool job: a nested
    `parallel_for` there degrades to the ~290 ms serial path, see `JobSystem.h`); a GPU
    bake as Metal's `EncodeSkyReflCube` already does for the reflection cube.
  - Metal runs `BuildFrameClusterLights()` twice per frame in deferred mode
    (`MetalRenderer.mm` ~14246 and ~15091). Build once into a member and reuse it.
  - REN-104 (D3D11/D3D12 build the clusters twice per frame) is not a plain duplicate:
    the second call carries the GI flag (mask channels). It is allocation-free now.
- **Traps:**
  - `MetalRenderer.h` (include `:18`, member `:642`) and `MetalRenderer.mm` (`:12593`,
    `:13748`, `:13862`, `:7684`) are also edited on topic 216's branch (around `:8185-8640`).
    Expect a textual merge, not a conflict of intent.
  - A new `.cpp` in HE_Rendering's shared helpers must also go into
    `tests/CMakeLists.txt`'s fixed list.
  - A doctest `MESSAGE` with a `const char*` member of a local struct printed `1`; use
    `std::string`.
  - `globalPool()` is in the global namespace, not `HE`.
- **Next steps:**
  1. If Windows CI fails at D3D11 `:2628`/`:4621`/`:6323` or D3D12 `:5797`/`:8972`/
     `:10308`, check the `SkyEnvBake.h` include and the impl-struct members
     (`m_skyEnvBaker`, `frameClusterScratch`).
  2. Its own step: take the sky bake off the render thread or onto the GPU (above). That
     is where the day-night stall is.
  3. Metal: one cluster build per frame (above), together with topic 216's Metal work.
