# Editor remembers window size, position, maximized (2026-10-21)

The editor saves its window rectangle at shutdown (`config.json`: `WindowX/Y/W/H/Maximized`)
and applies it at the next start.

## Handoff

**Done**
- Pure logic, no SDL: `src/HE_Editor/EditorWindowPlacement.h`
  - `resolveWindowPlacement(saved, displays)`: saved rect vs. usable display bounds; a rect whose
    title strip is not on any display falls back to centred on the primary (size kept, shrunk to fit).
  - `WindowPlacementTracker`: remembers the last NOT maximized/fullscreen/minimized rect, only once
    stable for 4 frames (maximize animation frames never count); a minimized maximized window stays maximized.
- Hook: `WindowProps::resolveRect` + `restoreRect/restorePosition/x/y/startMaximized`
  (`src/HE_Core/include/Window/Window.h`). `Window::Init` calls it after `SDL_Init` (displays known),
  skips the content-scale multiply/clamp for a restored size (saved values are already SDL window
  coords: points on macOS, pixels on Win/X11), positions the hidden window, maximizes it before the splash ends.
- Editor: `EditorApplication::GetConfig` installs the hook (`loadSavedWindowPlacement`,
  `usableDisplayRects`), `OnRender` feeds the tracker, `writeEditorConfig` writes the keys.
- Not in hidden mode (`hiddenWindowRequested()`): neither read nor written. Wayland: position not set.
- Test: `tests/test_editor_window_placement.cpp` (10 cases, 35 asserts) - passed standalone
  (`clang++ -Itests/vendor -Isrc/HE_Editor` + doctest main).

**Verified**: standalone doctest run; `-fsyntax-only` of EditorApplication.cpp, Window.cpp,
Application.cpp with the real compile commands (rc 0).

**Not verified**: full `he_tests` / `HorizonEditor` link (build dir was emptied), a real
start/quit/start cycle on screen, Windows/Linux behaviour, a real monitor unplug. macOS maximize
before show (`SDL_MaximizeWindow` on a hidden window) is untested visually.

**Next steps**
1. Run the editor once: resize, maximize, quit, check `config.json`, start again.
2. Windows/Linux: confirm maximized-before-show works and no flash at startup.
3. Optional: also reapply when a display layout changes at runtime (not done).
