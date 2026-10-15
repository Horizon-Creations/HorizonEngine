# Development stats tile (F10) — 2026-10-15

In a **development build** of the game (Debug / RelWithDebInfo, i.e. where `HE_GAME_DEV_HOTKEYS` is
defined — see `src/HE_Game/CMakeLists.txt`) **F10** shows a card in the top-right corner with the
frame counters the editor's viewport overlay has (FPS, GPU ms, draws, triangles, objects, VRAM) plus an
**extended GPU block**: renderer + resolution, CPU time of the captured frame, and the GPU passes
sorted by cost (`~` marks an approximate sub-encoder interval). F10 again hides it. Shipping builds
(Release / MinSizeRel) do not compile the key in.

## How it works

- `Diagnostics/DevStatsTile.{h,cpp}` (HE_Core): `formatDevStats()` is a pure function (the text),
  `DevStatsTile` builds a UI widget in code (panel + multi-line text, ConstantPixel scale, top-right,
  not hit-testable), registers it as `mem://DevStatsTile.hasset`, shows/hides it and rewrites the text 4×/s.
- Per-pass GPU time exists only while the profiler records, so while the card is visible it keeps the
  profiler's live mode on (whole-frame GPU ms + counters) and requests **one detailed single-frame
  capture per second** (`EngineProfiler::requestSingleFrameCapture`); the pass list is that frame's.
  Cost: one serialized frame per second on tile-deferred GPUs. Hiding the card turns live mode off.
- `WidgetManager::setElementProp()` is new: set one property of one element of a live widget from C++.
- `GameApplication` (`HE_GAME_DEV_HOTKEYS` only): F10 in the key block next to V, `update()` at the top
  of `OnRender`, `shutdown()` before `m_widgets.clear()`.

## Handoff

- **Topic / step:** development stats tile, step 1 of 1
- **Done:** F10 card in development builds of the game with compact + extended GPU stats.
- **Files touched:** `src/HE_Core/{include,src}/Diagnostics/DevStatsTile.*` (new), `src/HE_Core/{include,src}/UIWidget/WidgetManager.*` (`setElementProp`), `src/HE_Game/src/GameApplication.{h,cpp}`, `src/HE_Core/CMakeLists.txt`, `tests/test_dev_stats_tile.cpp` (new), `tests/CMakeLists.txt`.
- **Verified:** `formatDevStats` cases (4) compiled and run by hand: pass. Everything else `-fsyntax-only` (with and without `HE_GAME_DEV_HOTKEYS`).
- **Not verified:** the widget cases of `test_dev_stats_tile.cpp` (toggle, update, layout in the top-right corner, `setElementProp`) — they run in CI only; **the game itself** (no GPU here): placement, readability, that the single-frame capture actually returns passes on each backend, and how much the 1 Hz capture stalls. Backends that do not fill passes (Vulkan/D3D12 may not) show "none reported by this backend".
- **Decisions taken:** game only (the editor already has Show ▸ Stats); F10 (user's choice); extended = per-pass times via a 1 Hz single-frame capture (user's choice); a widget, not ImGui (the game has none).
- **Not taken / suggested:** the editor overlay (`ViewportPanel.cpp drawStatsOverlay`) could call `formatDevStats` so both say the same thing; a config key to start with the card open.
- **Next steps:** 1. Run a Debug game, press F10, check the corner placement and the pass list on each backend. 2. If a backend shows no passes, see its `GetFrameGpuStats` and the `isRecording()` gating in its timing code.
