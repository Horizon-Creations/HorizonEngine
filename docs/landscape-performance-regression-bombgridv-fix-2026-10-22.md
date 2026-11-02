# Landscape regression: the V=1 variant cost of `heBombGridV` is gone (Thema 237, Schritt 3)

22.10.2026, same MacBook Air M5 / Catania scene / `--detailed` loop as `landscape-performance-regression-asset-attribution-2026-10-22.md`
(Metal, 5.2 MP, GI on, 200 warm-up + 600 frames, AC power, display held awake with `keepawake.sh`). Raw data: `perf-audit/raw-landscape-regression/d23*` (the `*.profile.json` of these runs, ~447k lines each, exist only locally and are left untracked, as for d22),
shader dumps: `perf-audit/raw-landscape-regression/shader-dump-d23/`.

## 0. Result

1. **Confirmed in the shader, and fixed.** `M_AutoLandscape` with the `Groups` pin wired (`8f3a6e47`) emits per read a runtime `V`, three clamped slice ints and `textureGrad` taps whose
   array slice comes from the hash. With the 5-slice cache arrays (V = 1) that always selects slice = layer, but the GPU pays for it: **SSAO column 8.42 ms -> 6.94 ms (-1.48 ms), sum of passes 24.55 -> 22.98 ms**
   (n = 3 each, interleaved, same build otherwise). That is below the 7.3 ms the step aimed at and indistinguishable from the "variants off" oracle within this series' noise (6.94 against 7.02; 6.93 against 7.01 in the earlier series).
2. **The fix:** a branch on `V == 1` around the reads of every Groups-wired array node. `V == 1` takes the plain taps (`vec3(t, <v>_l)`, the layer itself, text of the pre-Groups read);
   `V > 1` takes the slices the hash picked (unchanged code). `V` is derived from `textureSize(...).z`, so it is the same for the whole draw: a uniform branch, no divergence.
   `MaterialGraph.cpp`, `emitBombNode`. The Groups-unwired text is byte-identical (FNV pin in `test_auto_landscape_variants.cpp` still green).
3. **Not fixed, by design:** with arrays that really carry variants (V > 1) the old cost is back (the `V > 1` branch is the old code, and `V > 1` behaviour must not change). The 17-slice witness (section 4) shows it renders
   identically; its cost was not measured (no real V > 1 arrays exist yet).

| Cell (Metal, n = 3, interleaved) | Scene | SSAO | Σ passes (min..max) |
|---|---|---|---|
| `d23before`: variant shader as shipped (`8f3a6e47`) | 13.65 | **8.42** | 24.55 (24.47..24.56) |
| `d23after`: the fix | 13.57 | **6.94** | **22.98** (22.97..23.06) |
| `d23oracle`: same build, variants switched off (plain `heBombGrid`, the pre-Groups read) | 13.62 | 7.02 | 23.09 (23.07..23.15) |

   The earlier series with the experiment build (`d23m0..m3`, n = 3 each, interleaved) agrees: m0 8.41 (shipped), m1 7.01 (variants off), m2 8.38 (no per-tap clamp), m3 6.93 (the branch).
   Σ ranges of the cells do not overlap (before 24.47..24.56 against after 22.97..23.06). Absolute values are only comparable inside this series (attribution doc: cur 25.41 with an older build of the same engine).

## 1. The shader dump (the evidence step 3 asked for)

`HE_DUMP_AUTOLAND_SHADERS=<dir> he_tests --test-case="Dump the auto landscape shaders"` (new, opt-in, in `test_auto_landscape_variants.cpp`) writes the auto material's fragment text:
lit + G-buffer GLSL and the lit one cross-compiled to MSL and HLSL. **The text does not depend on V**: `V = textureSize(...).z / Groups` is evaluated by the GPU, so "V=1 against V=4" is the same shader text with different texture depth;
the diff that matters is variants against no variants (`lit.groups-off.msl` = Groups unwired, i.e. before `8f3a6e47`; `lit.variants-before.msl` = as shipped; `lit.fix.msl` = fixed):

* `heBombTap` -> `heBombTapV` (+ `out uint sel`, `sel = h.x & 255u`), `heBombGrid` -> `heBombGridV` (+ `out uint3 sel`): 2 lines of arithmetic. **Not the cost.**
* per array read (12 of them: 4 layers x Albedo/Normal/Mask), in the main body: `n8_V` from `get_array_size() / 4` (integer divide by a constant), three
  `n8_sK = clamp(int(n8_l) * n8_V + int(((gs.K & 255u) * uint(n8_V)) >> 8u), 0, get_array_size() - 1)`, and the three taps take `float(n8_sK)` as array index instead of the uniform `n8_l`.
  Texture-size queries in the MSL: 14 (Groups off) -> 62 (variants).
* MSL size 57.6 KB (Groups off) -> 67.8 KB (variants) -> 77.5 KB (fix, both branches in the text). HLSL 54.3 -> 65.5 -> 73.5 KB.

**Which part costs** (experiment build, `HE_EXP_BOMBV`, since removed; the shader text of mode 3 is byte-identical to the shipped fix, `cmp` on MSL and HLSL):

| mode | what | SSAO (n=3) |
|---|---|---|
| 0 | shipped | 8.41 |
| 1 | codegen ignores Groups (plain `heBombGrid`) | 7.01 |
| 2 | base slice clamped once per read, no per-tap `clamp` | 8.38 (no gain) |
| 3 | `if (V == 1)` around the reads (the fix) | 6.93 |

So the ALU of the slice maths is not what costs (dropping 36 clamps changes nothing). What costs is that the array slice of the taps depends on the hash; a branch that keeps the V = 1 taps free of it recovers everything.
Why the Metal compiler/GPU is slower with a hash-dependent slice (it always equals the layer) was not investigated further (no GPU counters); measured, not explained.

## 2. Pixel check (Metal and OpenGL, headless `HE_DUMP_AUTOLAND`, `px_autoland.sh`)

Deterministic frame (GI/AA/SSAO off, private HOME with the real 5-slice cache arrays; two runs of one binary are bit-identical; sensitivity control: bombing off differs in 2.1 M of 2.7 M bytes, max 152):

| pair | differing bytes (of 2 764 800), max |
|---|---|
| Metal, shipped shader vs fix | 2, max 1 |
| Metal, shipped vs variants off | 1, max 1 |
| OpenGL, shipped vs fix | 1, max 1 |
| OpenGL, shipped vs variants off | 0 |

(1 to 3 bytes of +-1: float contraction differences of a recompiled shader.)

## 3. Tests and compile checks

* `test_auto_landscape_variants` (8 cases, 8 358 assertions) and `test_material_graph` (104 cases) pass, including the FNV pin of the Groups-unwired text and the cross-compile of the variant helpers and the whole auto
  material for Metal, GLSL 4.10, GLSL ES 3.00, GLSL 4.30, HLSL and SPIR-V. Counts in the variants test changed on purpose: `textureGrad(heTexP` is 12 (grouped graph) and 72 (auto material) in the *text* now (once per
  side of the branch; still 6 and 36 reads per pixel), plus `_V == 1) {` = 2 / 12 and `_s0))` = 2 / 12 (the slice is read on the `V > 1` side only).
* Full `he_tests`: 5681 of 5684 pass. Failing: `Disk registry: scanContentDirectory ...` (`test_hpak.cpp:1188`, 6 == 2) **fails identically on the unchanged tree** (rebuilt without the patch, same failure);
  two `test_profiler.cpp` cases failed in the first full run (tests dir as cwd) and pass from the build dir. The hpak case writes `/tmp/he_registry`, which other bees' parallel `he_tests` runs share, but it also fails when run alone here, so that is not the whole story. None touch the material code.
* HLSL: the generated `lit.hlsl` goes through `glslangValidator -D -V -e main -S frag` and `spirv-val` clean; a deliberately broken copy is rejected (negative control). Not `fxc`/`D3DCompile`.

## 4. V > 1 still the same shader behaviour

`landscape_tex_gen <dir> --variants 4 --arrays-only` (synthetic 17-slice arrays, 128 px) as cache arrays: Metal shipped vs fix **0 bytes** differ, OpenGL 3 bytes of +-1; the same frame with variants off differs in
2.16 M bytes (max 212), so the witness sees the variants.

## Handoff

**Done (verified):** shader dump (`shader-dump-d23/`, section 1); cost source narrowed by experiment (not the slice ALU; the hash-dependent slice); fix in `MaterialGraph.cpp` (`emitBombNode`); Metal SSAO 8.42 -> 6.94 ms, Σ 24.55 -> 22.98 (n = 3, interleaved, AC);
Metal and OpenGL pixel check at V = 1 and V = 4; variants test + material graph test green, FNV pin untouched; HLSL through glslang + spirv-val; full `he_tests` (one pre-existing failure, section 3).
**Not verified:** the exported/packed game (all runs are the editor loop; no export was built or measured); D3D11, D3D12, Vulkan on hardware (cannot run on this Mac; they get the same GLSL, which cross-compiles to HLSL/SPIR-V in the test; no FXC/DXC run); the cost with real V > 1 arrays; why the hash-dependent slice is slow;
the Windows player's backend (request 146). The 8f3a6e47 *asset* was left unregenerated here, and that was wrong for packed builds: the hasset embeds baked GLSL, `ContentManager::loadAsset` regenerates it only in the editor, but `HpakWriter` ships the text
stored in the file (see `landscape-performance-regression-baked-text-2026-10-22.md`, which regenerated the asset and pins it with a test). The runs above used the regeneration path, so the editor numbers stand; an exported game was not measured.
**Decisions:** branch around the reads instead of cheaper maths (measured: only the branch helps); the `V > 1` side stays the old code; the opt-in shader dump lives in the variants test; experiment toggle removed again.
**Next steps:** (1) Oct 8 -> Oct 10 engine Scene +1.8 ms (step 4 of the topic); (2) when real V > 1 arrays ship, measure their cost (the `V > 1` side costs what `8f3a6e47` cost, ~1.4 ms Metal): idea, compute `V` and the three slices once per
layer instead of per node (Albedo/Normal/Mask of a layer share the grid; their arrays have the same depth), or a lower-precision variant pick; (3) the remaining 0.36 ms of `0d7cdf09` (puddles, SSAO +0.23).
**Pitfalls:** `timeout` does not exist on macOS (use `perl -e 'alarm N; exec @ARGV'`); the perf runner waits for a quiet machine (other bees' clang/he_tests delay a cell by minutes); `landscape_tex_gen`'s arrays have to sit in `<HOME>/Library/Application Support/HorizonEngine/EngineContentCache/Textures/Landscape`
(`Textures/Landscape`, not `Textures`) of the private HOME or the witness silently renders the white fallback; a witness with GI/AA on is not repeatable (two runs of one binary differ).
