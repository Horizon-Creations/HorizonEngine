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

## Handoff

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
