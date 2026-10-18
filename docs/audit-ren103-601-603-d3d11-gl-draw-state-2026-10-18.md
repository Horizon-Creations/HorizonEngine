# REN-103 / REN-601 / REN-603: D3D11 and GL draw state (Thema 217, step 2)

Audit entries: `docs/audit/modul_rendering.md`, REN-103 (D3D11 graph-material draw binds
and restores ~45 registers per draw), REN-601 (GL fills the whole lighting block per
material draw and throws it away) and REN-603 (GL UI: one draw + 15 `glUniform*` per quad).

## What changed

**D3D11, REN-103** (`src/HE_Rendering/src/Backends/D3D11/D3D11Renderer.cpp`, block
"Graph-material runs (REN-103)" above `drawDC`):

* The frame-constant half of a material draw goes on at the first material draw of a
  *run*, in `bindMatRunState`: HeLighting b0/b8, U/HeParams b1/b3/b9, samplers s0/s2/
  s4..s7, the DDGI atlases t17/t18/s1/s3, sky cube + AO t15/t16/s15. It comes off in
  `endMatRun`, the old restore list verbatim, in one place.
* `endMatRun` runs at the start of every built-in draw, and in `drawLoop`, which every
  pass over a DrawCall list now goes through (forward, G-buffer, deferred replay,
  transparent). So no run stays open into the skinned pass, the decals, the TAA velocity
  pass or the deferred resolve.
* Per draw: VB/IB, t2 heTex0, t4..t7 heTexP, the t14 weightmap, the U upload, and VS/PS/IL
  only when the shader differs from the run's last one (the instanced twin updates the
  tracking).
* HeParams is mapped once per DrawCall instead of once per instance.
* Kill switch `HE_D3D11_MAT_STATE_EAGER=1` ends the run after every draw: the old per-draw
  sequence, same binary.
* Witness lines:
  * `D3D11Renderer: graph-material draws N in M state runs` (up to 8 per session).
  * `HE_D3D11_MAT_STATE_TIMING=<frames>` logs the draw loops' CPU median.

**GL, REN-601 (+ units)** (`OpenGLRenderer.cpp`, forward loop):

* `Lighting lit{}` + `fillMatLight` moved inside `if (!m_matLightUploadedThisFrame)`.
* Units 14..17 (sky cube, AO, DDGI atlases) are bound once per loop, at the first material
  draw (`matUnitsBound`). The transparent pass does the same with `tpMatUnitsBound`.
* Nothing else in either loop binds those units (checked by grep).

**GL, REN-603** (`kUIVS`/`kUIFS`, `RenderUIPass`, `OpenGLRenderer.h`):

* The per-quad values are ten vec4 instance attributes (`UIQuadGL`, `m_uiInstVAO`/`VBO`)
  and reach the fragment stage as `flat` varyings.
* `kUIFS` `#define`s the old uniform names onto them, so its body is unchanged and the
  D3D/Vulkan/Metal/software ports need nothing.
* A batch is consecutive solid/glyph/image quads under one clip and one unit-0 texture,
  drawn with one `glDrawArraysInstanced`. It is flushed before a clip change, a texture
  change, any material quad (before its backdrop snapshot) and at the pass end.
* Every resolve that can upload (UI material program, font atlas, UI image) runs in a
  pre-pass. `UIFontAtlasTexture` and `ResolveUITexture` bind the new texture on the active
  unit, which would corrupt a pending batch.
* Kill switch `HE_GL_UI_BATCH=0` draws one quad per draw.
* Witness line: `OpenGLRenderer: UI pass N quads in M draws`.

**Witness scenes** (`EditorApplication.cpp`):

* `HE_DUMP_MATGRID=<n>[,mix]` adds n graph-material spheres behind the material test
  sphere, one mesh each. `mix` leaves every other one built-in, and because the opaque list
  is sorted by random mesh ids the runs then end at built-in draws.
* `HE_DUMP_UITEST=text` = image + glyphs + a clipping panel.
* `HE_DUMP_GPUTIME` also logs the CPU time of `Render()`.

**Source pin:** `tests/test_culling.cpp`, "D3D11/GL material draw state once per run, UI
quads batched (REN-103/601/603)".

## Before / after

Call counts come from the code. A D3D11 forward material draw, one instance:

| | Before | After |
|---|---|---|
| D3D11 context calls per material draw | 44 (20 bind + 7 upload/CB + 1 draw + 16 restore) | ~10 in a run (13 when the shader changes) + 31 per run |
| MATGRID 256: 241 material draws | 241 runs, ~10 600 calls | 1 run, ~2 450 calls |
| MATGRID 256,mix: 121 material draws | 121 runs, ~5 300 calls | 57–66 runs, ~3 300 calls |
| GL `fillMatLight` per frame (forward) | one per graph-material draw | 1 |
| GL unit 14..17 binds per frame (forward) | 9 calls per graph-material draw | 9 |
| GL UI, `UITEST=text` (209 quads) | 209 draws + 3 135 `glUniform*` | 4 draws, 0 `glUniform*` per quad |
| GL UI, `UITEST=image` (15 quads) | 15 draws | 2 draws |

Measured on NN-WS03 (RTX 4070), same binary, interleaved A/B/A/B, three pairs each. The
GPU was 88 % busy with the human's game, so whole-frame times are GPU-bound.

| Measurement | A (new) | B (old path) |
|---|---|---|
| D3D11 draw loops CPU, MATGRID 256 (241 draws), median ms | 0.2171 / 0.2179 / 0.2188 | 0.2662 / 0.2684 / 0.2785 (EAGER) |
| D3D11 draw loops CPU, MATGRID 256,mix (121 draws), median ms | 0.1362 / 0.1363 / 0.1387 | 0.1473 / 0.1476 / 0.1492 (EAGER) |
| D3D11 whole `Render()` CPU, MATGRID 256, median ms | 14.1–16.1 | 13.7–14.2 — GPU-bound, no signal |
| GL whole `Render()` CPU, `UITEST=text`, median ms | 0.162 / 0.163 / 0.165 | 0.327 / 0.329 / 0.330 (`HE_GL_UI_BATCH=0`) |

* **D3D11:** about −20 % of the draw-loop CPU with long runs and −7 % with short ones.
  In absolute terms that is ~0.2 µs per material draw. The NVIDIA D3D11 driver makes these
  calls far cheaper than the audit's 1–3 ms estimate; the win scales with draw count and
  with weaker drivers.
* **GL UI:** the UI-only frame halves.
* **GL REN-601:** no CPU number. The pre binary has no CPU line and no kill switch, so
  only the count is reported.

## Pixel A/B

Script: `scripts/mat-state-ab/cap103.ps1`, diffed with `scripts/d3d12-static-heap/diff202.py`.

* **pre:** the C:/hw217 deploy at a8d45437. **new:** this step. **eager:** the same binary
  with `HE_D3D11_MAT_STATE_EAGER=1`.
* All captures: `HE_SKY_TIME=0`, 16 settle frames.
* Band 0 holds the animated sky and is noise on D3D11.

| Scene | Comparison | Result (mean abs diff, non-sky bands) | Noise floor |
|---|---|---|---|
| GL `UITEST=image` | pre vs new | md5-identical | — |
| GL `UITEST=text` | new vs `HE_GL_UI_BATCH=0` | md5-identical (209 quads, 4 vs 209 draws) | — |
| GL MATERIALTEST, forward / translucent / GI | pre vs new | 0.0000 / 0.0000 / ≤ 0.0007 | 0.0000 / 0.0000 / ≤ 0.0006 |
| GL MATERIALTEST translucent, deferred (`RENDERPATH=1`) | pre vs new | 0.0000 | 0.0000 |
| D3D11 MATERIALTEST, forward / deferred / translucent | pre vs new | ≤ 0.0001 in every non-sky band | ≤ 0.0002 |
| D3D11 LANDSCAPELAYERS, GI off / on | pre vs new | 0.17–0.21 / 0.009 | pre vs pre 0.23 / 0.018 |
| D3D11 MATGRID 256 | eager vs new | 0.0000 | — |
| D3D11 MATGRID 256,mix | eager vs new | 0.0000 | new vs new 0.0000 |
| **Negative control:** MATGRID 256,mix without `endMatRun()` at the built-in draw | eager vs neg | **28.8 (bands 0–1 at ~57)**, spheres visibly wrong | |

The negative control matters. The plain MATERIALTEST and MATGRID scenes did *not* react
to it, because sorting by mesh id put every built-in draw before the material draws. Only
the `mix` scene has a built-in draw after an open run, so use it for any later change here.

## Handoff

- **Topic / step:** 217, step 2 of 2
- **Branch / commit:** claude/audit-optimierung-d3d11-d3d12-opengl-backend-stufe-3 @ 877887ec + this doc update (pushed; the CI link is in the Hive post)
- **Done:** D3D11 binds graph-material frame state once per run of material draws and restores it once. HeParams is mapped once per DrawCall. GL fills the material lighting once per frame and binds units 14..17 once per loop. GL UI quads are instanced in batches.
- **Files touched:**
  - `D3D11Renderer.cpp`: `bindMatRunState`/`endMatRun`/`drawLoop` above `drawDC`; the witness and timing lines after the transparent pass.
  - `OpenGLRenderer.cpp`: `kUIVS`/`kUIFS`, the UI program setup, `RenderUIPass`, and the forward and transparent loops.
  - `OpenGLRenderer.h`: `UIQuadGL` and the UI scratch members.
  - `EditorApplication.cpp`: MATGRID, `UITEST=text`, the GPUTIME CPU line.
  - `tests/test_culling.cpp` (pin), `scripts/mat-state-ab/cap103.ps1`.
- **Verified:**
  - The pixel A/B tables above, including the negative control.
  - Witness lines: N draws in M runs; UI quads in draws.
  - `he_tests` full Release run, scratch APPDATA: 5524 of 5529 cases pass. The 5 failures are not caused by this step:
    - 3 are `test_input_gamepad` (the real Xbox pad on NN-WS03, known noise).
    - 2 are `test_profiler` (`isRecording`, `recordedFrames`). They pass 3/3 when run alone (`--source-file=*test_profiler.cpp`), so they are order-dependent in the full run; this step does not touch EngineProfiler.
  - The pin test: 47 assertions.
  - `scripts/validate_embedded_shaders.py --glsl-only`: 58/58, `kUIVS`/`kUIFS` included.
- **Not verified:**
  - **GL with more than one graph-material draw was never compared against pre.** That is exactly the case REN-601 and "units once" affect, so state it plainly. Pre has no MATGRID and REN-601 has no switch. Both are proven only on single-draw scenes (forward, translucent, GI, deferred translucent: identical), plus a visual check of the GL `MATGRID=256,mix` frame.
  - Vulkan/D3D12/Metal are untouched by design.
  - GL on macOS / Mesa was not run. `scripts/validate_embedded_shaders.py --glsl-only` (glslangValidator) accepts the new `kUIVS`/`kUIFS`; no driver other than NVIDIA ran them.
  - GL REN-601 CPU timing: no same-binary switch.
  - A clean GPU-time number: the GPU was busy with the human's game.
- **Decisions taken:**
  - REN-103 part 3 (sort the opaque pass by material) was skipped. The pass is sorted by mesh for good reasons, and material runs already cut most calls.
  - REN-603 went straight to stage 2. Stage 1 (uniform cache) would be dead code once the uniforms are gone.
  - The UI draws are not added to `m_counters.draws`, because tests read that counter.
- **Decisions suggested, not taken:**
  - The D3D11/D3D12/Vulkan UI passes are still one draw per quad. The same batching applies, but their audit entries are separate.
  - REN-602 (GL resolves and hashes material GLSL per draw) probably costs more CPU than REN-601 did.
- **Traps:**
  - Only `MATGRID=<n>,mix` exercises the run end at a built-in draw.
  - Python on this box needs `C:/` paths, not `/c/`.
  - `sed -i` from Git Bash turns a CRLF file into LF. Check the line endings before committing.
  - C: had 1–2 GB free.
- **Next steps:**
  1. Queen: merge into release/0.7.0 after CI. Topic 217's listed items are done.
  2. Optional follow-ups: REN-602 on GL; UI batching for the D3D11/Vulkan UI passes.
