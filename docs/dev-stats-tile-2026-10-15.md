# Development stats tile (F10) — 2026-10-15

In a **development game** **F10** shows a card in the top-right corner with the frame counters the
editor's viewport overlay has (FPS, GPU ms, draws, triangles, objects, VRAM) plus an **extended GPU
block**: renderer + resolution, CPU time of the captured frame, and the GPU passes sorted by cost
(`~` marks an approximate sub-encoder interval). F10 again hides it. V toggles VSync (same gate).

A game is a development game in either of two ways (`GameApplication::m_devHotkeys`):
1. the executable was built with them: Debug / RelWithDebInfo, or `-DHE_GAME_DEV_HOTKEYS=ON`
   (`src/HE_Game/CMakeLists.txt`), or
2. its export asked for them: **Export > "Development hotkeys"** (`ExportProfile::devTools`, on in the
   seeded *Development* profile, off in *Shipping*; a profile saved before the key existed is read by
   its name) is written to `project.hcfg` (flag bit 4096, `ProjectConfig::devTools`). This is what gives a
   game exported from a Release editor, which copies the Release runtime, the keys. Applications never get it.

## "F10 does nothing" — what the log says (`HorizonEngine.log` next to the exe)

Startup, always one of:
- `GameApplication: development hotkeys ON (V = VSync, F10 = stats card)`
- `GameApplication: development hotkeys OFF - export with the Development profile's "Development hotkeys" ticked, or build with …` — this game has no F10: it is a Release runtime and its export did not ask for the keys (an export made before this existed has no flag in `project.hcfg`: export again). The banner's `Build :` line is the ENGINE's configuration, not HorizonGame's.
(Before the export flag the line read `compiled in` / `NOT compiled in`.)

On F10: `GameApplication: stats card ON` / `OFF` / `could not be created (see the DevStatsTile line above)`; then
`DevStatsTile: widget N built`, `DevStatsTile: first refresh written (N lines, widget N visible=1)` once per showing,
or `DevStatsTile: could not write the card (text=…, size=…, widget … alive=…)` / `…widget could not be created` (preceded by WidgetManager's own "asset not found" / "invalid widget tree" line).
No `stats card` line at all on F10 = the key never reached `OnEvent` (build without the keys, or a text field has the keyboard).

## How it works

- `Diagnostics/DevStatsTile.{h,cpp}` (HE_Core): `formatDevStats()` is a pure function (the text),
  `DevStatsTile` builds a UI widget in code (panel + multi-line text, ConstantPixel scale, top-right,
  not hit-testable), registers it as `mem://DevStatsTile.hasset`, shows/hides it and rewrites the text 4×/s.
- Per-pass GPU time exists only while the profiler records, so while the card is visible it keeps the
  profiler's live mode on (whole-frame GPU ms + counters) and requests **one detailed single-frame
  capture per second** (`EngineProfiler::requestSingleFrameCapture`); the pass list is that frame's.
  Cost: one serialized frame per second on tile-deferred GPUs. Hiding the card turns live mode off.
- `WidgetManager::setElementProp()` is new: set one property of one element of a live widget from C++.
- `GameApplication` (when `m_devHotkeys`): F10 in the key block next to V, `update()` at the top
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
