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

## Handoff

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
