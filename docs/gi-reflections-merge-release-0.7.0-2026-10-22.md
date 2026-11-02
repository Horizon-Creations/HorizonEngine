# GI reflections: merge of origin/release/0.7.0 (topic 191, step 13)

PR #183 (`claude/global-illumination-reflections` -> `release/0.7.0`) conflicted with
release after PR #182 (topic 207, material tessellation/displacement) landed. This is a
plain merge of `origin/release/0.7.0` @ `8d091257` into the topic branch (no rebase, no force).
While I worked release advanced to `8da78bce` (two Planner commits, only `src/HE_Editor/Planner*`
and planner tests); that was merged as a second merge commit, and a third one for `9a7323d0` (Planner W4; also touches
`EditorApplication.cpp`/`EditorHelp.cpp`, auto-merged, `editor_help_audit.py --check` still passes with 2032 entries). No conflicts in either.

## Conflicts

Exactly one file conflicted (`git diff --name-only --diff-filter=U`):

| File | Hunk | Resolution |
|---|---|---|
| `src/HE_Rendering/src/Backends/D3D11/D3D11Renderer.cpp` (~7861, material draw setup) | Topic: `BindGIReflFwd(ctx, giReflSRV, ...)` (t32/s9). Release: hull + domain shader binding block (`matTess`, `HSSet*/DSSet*`, topology 3-control-point patch). | Both kept, topic's block first, then release's. They are independent: the DS binds only b0/b1/b3, t2/t4..t7/t14, s0/s2/s4..s7; t32/s9 is a PS-only bind. Release's matching unbind (`if (matTess)`, ~7981) and the topic's restore (`BindGIReflFwd(..., giReflSRV, ...)`, ~8249) are both intact. |

Everything else auto-merged (ci.yml, he_vk_imagetests.py, EditorApplication.cpp, the
D3D11/D3D12/Vulkan/OpenGL/Metal renderers and headers, tests/CMakeLists.txt,
tests/test_culling.cpp). Semantic overlap was checked by reading, see Verified.

**Nothing of topic 191 had to give way to release.**

## Handoff

- **Topic / step:** 191 Global Illumination Reflections, step 13 (merge with release/0.7.0)
- **Branch / commit:** `claude/global-illumination-reflections`, merge commit (see git log), pushed: yes
- **Done:** release/0.7.0 is merged in; the branch has both parents, no conflict markers.
- **Files touched:** `src/HE_Rendering/src/Backends/D3D11/D3D11Renderer.cpp:7861-7894` (the only manual resolution); this doc.
- **Verified (macOS, Release, `out/build/release`, Ninja):**
  - `he_tests` builds (rc 0, 1659 steps). `he_tests --source-file=*<f>.cpp` all green: test_culling (68 cases, includes the "DDGI probe sampling" `kGiReflCS` drift subcase), test_material_tessellation (13), test_vulkan_upload_alloc (6), test_landscape_reload (5), test_material_graph (104), test_engine_materials (12).
  - `scripts/validate_embedded_shaders.py`: GLSL 58/58 compile; `editor_help_audit.py --check`: 1236/1236, 0 missing.
  - `VulkanRenderer.cpp` `-fsyntax-only` against MoltenVK headers (HE_VULKAN_ENABLED) is clean; negative control (renamed `ensureGiReflTargets` in a copy) errors as expected.
  - GI-related symbol counts (`GIRefl|giRefl`) in VulkanRenderer.cpp (148), D3D12Renderer.cpp (173), D3D11Renderer.cpp (112) are identical before and after the merge. `GIReflectionTier.h`, `scripts/runtime_size.py`, `scripts/validate_embedded_shaders.py`, `docs/gi-reflections-*` have no diff against the pre-merge branch.
  - `HorizonEditor` was NOT linked: the target also builds the engine SDK release lib set (a second full engine build, ~740 steps), which I stopped. Instead `EditorApplication.cpp.o` (the auto-merged editor file with the capability gate) was compiled on the final tree, rc 0, 0 errors. The he_tests run above was on the tree after the first merge (before the Planner merge); the Planner merge did not touch any GI or renderer file, and was not re-tested.
- **Not verified:** D3D11/D3D12 compile (no toolchain on the Mac; Windows CI is the check); HLSL validation (fxc missing here, so the HLSL half of `validate_embedded_shaders.py` was skipped); MSL (metal toolchain missing); no GPU/visual run of Vulkan or D3D after the merge; Vulkan layout tracking (`heGIReflFwd` ping-pong) not re-run on device, only confirmed unchanged by symbol count and syntax check.
- **Decisions taken:** keep both sides in the D3D11 hunk; no changes to release's code.
- **Decisions suggested, not taken:** none.
- **Pending:** Windows/Linux CI on the merged head (push triggers the PR run); then the queen merges PR #183.
- **Traps:** a fresh worktree configure with `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` fails (glm::glm not a target); leave it off and pass the `FETCHCONTENT_SOURCE_DIR_*` of the main checkout. Configure took ~275 s, he_tests build ~45 min under load 20+.
- **Next steps:** 1. watch the CI run of the merged head (D3D11/D3D12/Vulkan compile on Windows). 2. If D3D11 fails, look first at ~7861-7894 and ~7981 in D3D11Renderer.cpp.
