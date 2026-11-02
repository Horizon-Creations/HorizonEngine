# Landscape regression: the committed M_AutoLandscape.hasset still baked the slow shader (Thema 237, Schritt 4)

22.10.2026, MacBook Air M5. Follows `landscape-performance-regression-bombgridv-fix-2026-10-22.md` (the `V == 1` branch in `emitBombNode`). No code of the fix changed; this step closes what that step left open.

## 0. Result

1. **A packed game did not get the fix.** `EditorDeps/EngineContent/Materials/M_AutoLandscape.hasset` held the *shipped* variant text of `8f3a6e47` (10x `heBombGridV(`, 0x `_V == 1`). `ContentManager::loadAsset` (~L684-697) regenerates the
   text from the graph, but only in the editor/loose-content path. `HpakWriter.cpp` ~L435-440 reads `customShaderFragGlsl` straight from the file's MTRL chunk and, ~L594, hands exactly that text to
   `compileShaderVariants` (`ExportDialogPanel.cpp` `CompileMaterialShaderVariants` -> `bakeMaterialShaderVariant`) for the PSHD blobs. A loaded packed material has `precompiledShaders` and is not regenerated either. So an exported game
   rendered the pre-fix shader (the +1.4 ms of `8f3a6e47`) until the asset is regenerated. The earlier handoff said "only if the exporter baked it from an unregenerated material" - the exporter never regenerates.
2. **Fixed: asset regenerated** (`landscape_tex_gen <dir> --material`, built from this tree; a data change). Now 10x `heBombGridV(`, 24x `_V == 1`. Only the baked GLSL text and the MTRL chunk length differ (90 954 -> 100 078 bytes);
   `strings` diff of old and new shows 51 changed lines, all shader text. The generator's own read-back check passes (writes, reloads, compares graph, GLSL, param names), the tripwire at `test_material_graph.cpp:3529` (graph == builder) stays green.
3. **New tripwire** `The baked GLSL in the shipped M_AutoLandscape.hasset is what the codegen makes today` (`test_material_graph.cpp`): the text a load regenerates must appear verbatim in the file bytes (a load always regenerates, so the stored text
   has to be read from the bytes), and the exporter's per-backend bake (`bakeMaterialShaderVariant`, ids 0..Metal: OpenGL, Vulkan, D3D11, D3D12, Metal) must succeed on it. Negative control: with the old asset (`git show HEAD:...`) the case fails; with the new one it passes.
4. No new Metal timing. The editor loop regenerates on load, so its numbers cannot change with the asset; the step-3 series (SSAO 8.42 -> 6.94 ms, sum 24.55 -> 22.98 ms, n = 3 each, interleaved) stands unchanged. The packed game was **not** measured (no export built).

## 1. Per backend (what is actually established)

| Backend | Status | Evidence |
|---|---|---|
| Metal | verified on hardware (step 3 timing + this step's pixels) | step-3 series; headless `HE_DUMP_AUTOLAND` with the regenerated asset: V=1 vs step-3 fix frame 0 bytes, vs shipped 2 bytes (max 1); V=4 (17-slice arrays) vs fix 0, vs shipped 0 bytes; V=1 vs variants-off oracle 3 bytes (max 1) |
| OpenGL (macOS 4.1) | verified on hardware (pixels + shader compiles; no GL timing taken) | same recipe: V=1 vs step-3 fix frame 0 bytes, vs shipped 1 (max 1), two runs of the new binary 0 bytes; V=4 vs fix 0, vs shipped 3 bytes (max 1); sensitivity: bombing off 2.11 M bytes differ, V=4 oracle (variants off) 2.17 M. Log: no `Cannot load asset`, no compile failure |
| Vulkan | cross-compiled only | `lib.fragment(..., B::SpirV)` and the exporter bake ok in the tests (full auto material); not run on a GPU; no spirv-val of the SPIR-V from the engine path, only of the HLSL route below |
| D3D11 | cross-compiled only | HLSL from the same text: dump `lit.hlsl` contains the `_V == 1` branch (12 lines), `glslangValidator -D -V -e main -S frag` + `spirv-val` clean, mangled copy rejected (negative control). Not `fxc`/`D3DCompile`, no hardware |
| D3D12 | cross-compiled only | same HLSL; not DXC, no hardware |

Windows-hardware steps stay open (memory: windows-steps-stay-open); nothing here marks D3D11/D3D12/Vulkan verified.
Shader dump (`HE_DUMP_AUTOLAND_SHADERS`): `lit.glsl`, `gbuffer.glsl`, `lit.msl`, `lit.hlsl` each carry the `V == 1` branch (12 `_V == 1` lines each) and 5 `heBombGridV` lines. The dumped GLSL is the material *body*, without the engine's lighting preamble, so standalone
`glslangValidator` on it fails at `heLight` (undeclared) - not a regression, only the assembled program (HLSL, or the test's in-process SPIR-V) is a valid unit.

## 2. Tests

`test_auto_landscape_variants` 8 cases / 8 358 assertions, `test_material_graph` 105 cases / 11 468 assertions (new case included) pass. Full `he_tests` (build dir): 5682 of 5685; failing `Disk registry: scanContentDirectory`
(`test_hpak.cpp:1188`, 6 == 2, fails on the unchanged tree too) and two `test_profiler.cpp` cases (`:107`, `:215`) that pass when run alone (18/18) and fail in the full run, as in step 3; none touch materials.

## Handoff

**Done (verified):** cause of the open question (packer ships the stored text: `HpakWriter.cpp` ~L439/L594, read); old hasset held the pre-fix text (grep: 10 `heBombGridV(`, 0 `_V == 1`); asset regenerated and committed (new text: 24 `_V == 1`); new tripwire test with a negative control;
OpenGL and Metal pixel check at V=1 and V=4 against the step-3 frames (`/tmp/he237s3/px`, numbers above); shader dump contains the branch in GLSL/MSL/HLSL, HLSL through glslang + spirv-val; `he_tests` as in section 2.
**Not verified:** an exported/packed game (no export built or measured; the claim "packed = fix" rests on the code reading plus the byte-level test); OpenGL GPU time (only pixels); D3D11, D3D12, Vulkan on hardware, no FXC/DXC, no spirv-val of the engine's own SPIR-V;
any Metal timing after this step (none taken, none needed for the editor loop); real V > 1 cost.
**Decisions:** regenerate with the project's generator instead of hand-editing; test reads the file bytes because a load always regenerates; Metal not re-measured (asset change cannot move the editor loop).
**Next steps:** (1) a Windows/D3D hardware run of the auto landscape, or an export + Metal/GL game measurement, to see the fix in a packed build; (2) Oct 8 -> Oct 10 engine Scene +1.8 ms (step 5 territory, not started); (3) the 0.36 ms of `0d7cdf09`;
(4) whenever `buildAutoLandscapeGraph` or `emitBombNode` changes, re-run `landscape_tex_gen EditorDeps/EngineContent/Materials --material` - the new test says so when it is stale. Any other generated EngineContent material (`mat_gen`, `matfn_gen`) can have the same staleness; this step only checked `heBombGridV`, which only the auto landscape asset contains.
**Pitfalls:** a load regenerates the GLSL, so `loadAsset` + compare tells nothing about the file; `landscape_tex_gen --material` needs no engine root (registers the weather builders itself); `out/deploy/Editor/EngineContent` is a copy made at build time and stays stale until copied (`cp`, not `mv`) - the editor runs read that one;
`px_autoland.sh` frames live in `/tmp/he237s3/px` (not committed, 3.6 MB each).
