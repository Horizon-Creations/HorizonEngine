# Water: parameter budget and wave tessellation, inventory (Thema 229 step 1)

2026-10-29, `claude/wasser-shader-wellen-optional-ber-tessellation-daf-r-mehr-ma` (contains `release/0.7.0` via 1bf9058f).
Survey only; no code changed. **V** = read in the file named, **N** = not verified (not built or run, or inferred).

## Result in one paragraph
The topic's premise is out of date. The wave tessellation was built on `release/0.7.0` (Thema 207's Displacement pin, a
Static Switch "Tessellation", per-body `TessWaves`) and is already on this branch. It needed **no** limit raise: the
tessellated permutation of the water uses **15 of 16** slots. Raising `kMatMaxParams` is not needed for this topic.
What is left is Metal (draws flat), the cost of the tessellated sheet, and a few unchecked edge cases (below).

## 1. The real budget (V unless marked)
* `kMatMaxParams = 16` (`MaterialGraph.h:614`). `paramSlot` (`MaterialGraph.cpp:890-901`) returns -1 over budget and the node
  bakes its default as a literal (no crash, the knob silently stops being a knob). UBO declared at `MaterialGraph.cpp:2140`.
* Water, tess OFF (= `Water.hasset`): **14 knobs** (`kWaterKnobs`, `test_engine_materials.cpp:98-113`; `kWaterParams`,
  `MatGen/main.cpp:1173-1177`), 2 free. `ShallowWater.w` is also a reserved, unused component (`water-shallow-params-2026-10-20.md`).
* Water, tess ON: **15**. `TessWaves` (Vec4, `MatGen/main.cpp:692`) exists only in the Tessellation permutation; the instance
  `WaterTessellated` is regenerated with the switch map (`WaterSurface.cpp:131-139`, `syncMaterialInstance`). 1 free.
  N: the count 15 is derived from the code, not printed by a run (nobody built it in this step).
  `tests/test_engine_materials.cpp:1455-1480` checks the permutation's displacement text but not its slot count.

## 2. What exists for displacement (V: `docs/water-tessellation-2026-10-27.md`, git log)
Commits 553a540c, c657145e, 5bde0c7e, 2d27f92a, 1a5d71c2, f3a0561e are ancestors of this branch (`git branch --contains`).
Per-landscape checkbox, per-body height/shore width, calm waves (peak ~0.16 m), flat coast, underwater fog follows the
surface per pixel on GL, Vulkan, D3D11, D3D12. Acceptance by the earlier worker: GL on macOS; the tessellated sheet itself on
Vulkan/D3D11/D3D12 on an RTX 4070 (doc, update 2026-10-28). So the old plan's step 3 is done and step 4 is mostly done.

## 3. If the limit is ever raised to N slots (not recommended now)
Hard-coded 16/64/256 outside the constant, all V. These are silent-wrong-read traps; only some have a static_assert.
| Place | What is hard-coded |
|---|---|
| `MaterialShaderLibrary.cpp:916` | WPO preamble `vec4 v[16]` (a second copy, NOT `kMatMaxParams`) |
| `ContentManager.cpp:313`, `ImporterCommon.cpp:951` | load rejects `paramFloats > 16*4`. On rejection the offset does not advance, so the tail (textures, names) is misread |
| `RenderExtractor.cpp:501-509`, `:88` | `block(64)`, `k < 16` in the override merge and `windWaterParams` |
| GL `OpenGLRenderer.cpp:3846` (`glBufferData(..,256,..)`), `.h:328` (`m_lastMatParams[64]`), `padded[64]` at 8404, 9653, 12507, 12780, 13137, 13406 | UBO size and 6 upload sites |
| Metal `MetalRenderer.mm` `padded/vpad[64]`, `size_t(64)` at 10081-10092, 13964, 14756, 15007, 15045, 15214, 16852, 16883 | 9 sites, `setFragmentBytes` (limit 4 KB, N<=256 ok) |
| D3D11 `D3D11Renderer.cpp:5251` (`makeCB(256,..)`), `:8203` | CB size and upload |
| D3D12 `D3D12Renderer.cpp:3923` `k_matSlot=256` (shared by U ring and HeParams ring), `:10036-10038`, `:12513-12524` | stride; N=32 needs a 512 B stride for HeParams |
| Vulkan `VulkanRenderer.h:505` `k_matSlotStride=256` (shared U + HeParams), `.cpp:3030`, `:9463-9476` | stride, dynamic offsets |
| MCP `McpToolsMaterial.cpp:1226` | warning text uses the constant (fine) |
| Tests | `static_assert(kMatMaxParams==16)` `test_engine_materials.cpp:828`, `:838`, `:855`, `:879`, `:921`, `:1028` (D3D reflection `Elements`), `test_material_graph.cpp:2951-3014` |
| `MatGen/main.cpp:1287` | budget check uses the constant (fine) |
HLSL/MSL come from SPIRV-Cross over the GLSL, so the array size flows through (N: only the WPO copy and GL/Vulkan are textual).
Cost of N=32 (arithmetic, N not measured): HeParams 256 -> 512 B per draw. D3D12/Vulkan rings 1024 draws x 2 frames x +256 B =
+0.5 MB per ring per backend if only the param ring grows; +1 MB if the shared stride doubles. Per-draw memcpy/memcmp
(GL dedupes with memcmp) 2x of a few hundred bytes: negligible next to a draw. Limits are not close: GL UBO >=16 KB,
D3D 64 KB CB, Vulkan dynamic UBO range >=16 KB. Serialization needs no version bump (length-prefixed `paramFloats`,
saved overrides are keyed by NAME, `SceneSerializer`, doc 2026-10-20); but a build older than the raise misreads a >16-slot
asset (see the load check above), so ship the raised runtime before any >16-slot `Water.hasset`. Editor UI: no cap found in
`MaterialEditorPanel.cpp` or `EditorHelp.cpp` (grep, N: not looked at on screen).
Proposed shape if done: one `kMatParamFloats = kMatMaxParams*4` / `kMatParamBytes` in `MaterialGraph.h`, every literal above
reads it, WPO preamble built from it, one test that greps the renderer sources for `[64]`.

## Handoff
* **Verified:** budget 16 (`MaterialGraph.h:614`), static_assert matches (`test_engine_materials.cpp:828`); Water 14 knobs tess off;
  the tessellation commits are on this branch; every hard-coded site in section 3 (file:line read). `grep` for `kMatMaxParams`,
  `k_matSlot`, `HeParams` in `src/HE_Rendering`, `src/HE_Core` matches section 3. No build, no test run, no screenshot.
* **Not verified:** slot count 15 by a run; Software renderer (no `HeParams` hit in its sources, N); editor panel on screen;
  per-backend cost numbers (arithmetic only).
* **Decision (suggested, queen decides):** do NOT raise the limit for this topic. Step 2 of the topic is not needed; step 3
  is already done on release/0.7.0; only a trimmed step 4 remains.
* **Steps 2-4 as sketched, replacing the old ones:**
  1. Close the topic if the open items below are accepted as separate tasks. Nothing else blocks the stated goal.
  2. Optional, own task: **Metal tessellation** (the switch does nothing on Metal; also the underwater plane there). This is the
     only gap of the five backends. SPIRV-Cross cannot emit HLSL tess stages (lesson on Thema 207), MSL can.
  3. Optional: **cost** of the tessellated sheet on the RTX 4070 and the M-series GL path (pass times on/off, `he_perf_capture.py`,
     `HE_DUMP_WATERTESS=1`), seams between two bodies, storm sea at the `maxDisplacement` 2.5 m clamp, packed-game fallback.
  4. Optional: Vulkan caustics still use the camera-local plane (128 B push block full, needs a small UBO).
  Raise `kMatMaxParams` only when a knob is added beyond slot 16; then follow the table in section 3.
* **Traps:** `ContentManager.cpp:313` rejects >64 floats silently; the WPO preamble `v[16]` is a second copy of the limit;
  D3D12/Vulkan share one 256 B stride between U and HeParams; `static_assert` in the water test is the only guard that fires.
