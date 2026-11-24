# Software renderer: materials, backdrop, sprite sheets, small text (2026-11-25)

Software projects ("Advanced Shader Effects" off) could not use materials at all. Now
widget materials run on the CPU, and the editor offers exactly the nodes that work there.

## Handoff

**What was done** (commits 032415e98, 51a70ed69, 18a5622a3, 1c80bb309, 7d4096e06 on
`release/0.8.0`)
- `HE::MatCpuProgram` (`src/HE_Core/include/MaterialGraph/MatCpuEval.h`): compiles a graph
  into an op list, applying generateFragment's rules. `matCpuSupports` is the node set.
- `SoftwareRaster.h`: `DrawOptions` + `MaterialResolver`.
  - A material quad is shaded by its program.
  - Backdrop works in phases: draw, snapshot with mips, carry on. The blur matches the GPU's.
  - Material textures use repeat addressing and sRGB decode.
- `SoftwareRenderer`:
  - Material cache keyed on graph JSON, instance switches and function JSON.
  - Parameters are bound by name every frame.
  - Time/Backdrop materials repaint the whole frame.
  - Clock is SDL ticks, as the GPU UI pass uses. `RasterizeUI(..., animated)` reports them.
- Editor, Advanced off:
  - Materials can be created and opened.
  - The add menu and function list show only CPU nodes.
  - New materials default to UI/unlit.
  - A "Software" toolbar cell names the first unsupported node.
- MCP: hook `cpuNodesOnly` replaces `materialsAllowed`. Node types are filtered,
  `add_node` refuses GPU-only nodes, and `create` defaults to the UserInterface template.
- `UIImage` sprite sheet: `Sheet Columns/Rows/Frames/FPS`.
  `UIElement::movesWithTime()` keeps an app awake.
- `HE::uiFontMips` + `kUIFontLodBias` (-1): GL and Metal upload the same levels the CPU
  samples.

**Verified**
- `test_mat_cpu_eval`: rules, plus all nine engine UI functions compile for the CPU.
- `test_software_raster`:
  - stripes equal a single thread, including material and backdrop;
  - backdrop blurs;
  - small-text error: 9 px 50.8 → 20.1, 12 px 40.9 → 14.7.
- `HE_DUMP_UITEST=materials`, Software vs Metal/GL:
  - eleven tiles within ±1;
  - Frosted Glass within ±9, because the Software capture is 1600×900 and the GPU's
    1280×720.
- `HE_DUMP_UITEST=text`: Software vs Metal/GL max 14, mean 0.008.
- Real export (Tamagochi copy, Advanced off, material built over MCP: texture × Engine
  Pulse): it draws on Software, every panel pixel changes between frames 40 and 100, and
  there is no fallback warning.
  - The export runs the "game" runtime with `backend=Software`.
  - The same pak on the locally built `app-basic` runtime (`scripts/build_runtimes.py
    --flavor app-basic`; log `runtime flavour 'app-basic'`, no shaderc): the material
    draws and moves, with no warning.
- A parameter change (`setMaterialParam`) repaints the next frame: `test_mat_cpu_eval`
  checks this through `RasterizeUI`.
- Sprite sheet through `WidgetManager`: isAnimating, two clock moments give two cells,
  and frame 0 is the top-left cell in pixels.
- Full ctest 383/383 and `editor_help_audit --check`.

**Not verified / known**
- **Exporter finding (separate from this):** an Advanced-off app on this Mac ships the
  game runtime even with Shipping ("game compiled from the release SDK"). The AppBasic
  bundle next to the editor is never used. See `findRuntimeBundle` vs. the
  SDK-compile path in `ProjectExporter.cpp`.
- Frosted Glass ±9: the different capture size (1600×900 vs 1280×720) is the likely
  cause, not a proven one.
- Vulkan/D3D: no font atlas mips (no Pixelated, no UI preview either), so small text
  there now looks different from Metal/GL/Software.
- Windows CI for these commits had not finished. The last finished run (designer +
  RendererFactory stub) was green for this work; `test_physics` and `test_animation_ik`
  were already red before it.
- Material textures on the CPU always read the base level. There is a `shortcut:` comment
  in `SoftwareRaster.cpp`; the GPU uses mips.
- Time materials in an event-driven app animate only when the app redraws anyway (on the
  heartbeat). That matches the GPU path, but it is a limitation of both.
- A UI material shows a texture upside down (vUV top-left, textures stored bottom-up) on
  all backends equally. This predates the change and was not changed.
- No full-window material timing was measured.
- Not looked at in the running editor window.
- Sprite sheets are not animated in the designer (it shows frame 0).

**Next steps**
1. Material texture mips on the CPU: lod from the UV derivative per quad row.
2. Decide whether UI materials should flip V for textures (affects GPU and CPU together;
   needs an old-content check).
3. Optionally let Time materials request frames (`movesWithTime` for material quads).
