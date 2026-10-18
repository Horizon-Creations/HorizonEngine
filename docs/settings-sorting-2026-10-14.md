# Engine Preferences vs. Project Settings — resorted (2026-10-14)

Rule: **what goes with the game or the team → `Config/ProjectSettings.json`; what is only
about the user or the machine → editor `config.json`; mixed → the project's value, with a
preview override for the user.**

## What moved

| Was (Preferences) | Is now |
|---|---|
| Rendering ▸ Display (backend, VSync, FPS cap) | Preferences ▸ Preview ▸ Editor Renderer |
| Rendering ▸ Post-Processing / GI / Effects, Display's AA + render path | Project Settings ▸ Rendering ▸ Quality / Post-Processing / Reflections & GI (new `graphics` block); Preferences ▸ Preview ▸ Viewport Quality holds the *user's own* values |
| Gamepad deadzones (Viewport page) | Project Settings ▸ Input ▸ Gamepad (`input` block) + Preferences ▸ Viewport stays for the user's own |
| Source Control ▸ Repository section | Project Settings ▸ Project ▸ Source Control (`EditorSettingsPanel::DrawRepositorySection`) |
| Preferences groups General/Editor/Rendering | Editor / Preview / Accounts & Connections / Tools |
| Project Settings groups Game/Rendering/Physics/Audio | Project / Rendering / Input / Physics / Audio / Online |

## Mechanism

* `HE_PROJECT_GRAPHICS_FIELDS(X)` (`ProjectSettings.h`) is the single field list: struct,
  equality, json, clamp and the `EditorConfig` twin are all expanded from it.
* `ProjectGraphics::enabled == false` (default) is the old behaviour exactly: export writes the
  exporting editor's values, viewport previews with the user's.
* `enabled == true`: export writes the project's values (`ExportDialogPanel`), and
  `EditorApplication::syncGraphicsSource()` swaps the project's values into `EditorConfig` each
  frame unless `PreviewQualityMode` says "My own values". The user's values are kept in
  `m_userGraphics` and restored at shutdown so config.json never stores a project's look.
  `EditorConfig::GraphicsFromProject` locks the catalogue rows while the project's are showing.
* The project pages draw the Preferences catalogue rows (`DrawEngineSettings` +
  `DrawOptions{rows, config}`) over a scratch `EditorConfig`, capture changes into the block and
  write once no widget is active (`ProjectSettingsPanel.cpp: drawGraphicsPage`).
* Input: `ProjectInputSettings` deadzones apply to the game in play mode
  (`EditorApplication::playStickDeadzone/playTriggerDeadzone`) and in the exported game
  (`GameApplication.cpp`).

## Handoff

- **Topic / step:** settings sorting, all three phases of the proposal
- **Branch / commit:** release/0.7.0 (pushed: see git log)
- **Done:** pages and nav restructured on both tabs; project graphics + input blocks; preview
  override; export + game honour them; repository section on the project tab.
- **Files touched:** `src/HE_Core/include/Project/ProjectSettings.h` (field list),
  `src/HE_Editor/EditorGraphics.{h,cpp}`, `EditorApplication.cpp` (`syncGraphicsSource`,
  `playStickDeadzone`), `EditorSettingsPanel.cpp` (pages, `DrawOptions`, `pageRows`,
  `drawViewportQualityHeader`), `ProjectSettingsPanel.cpp` (`drawGraphicsPage`, `drawInputPage`,
  `drawSourceControlPage`, nav), `ExportDialogPanel.cpp`, `GameApplication.cpp`,
  `EditorHelp.cpp` (new "Project Graphics/", "Input/", "Viewport Quality/" entries).
- **Verified:** standalone harnesses (no CMake build): `test_project_settings`,
  `test_editor_graphics`, `test_project_settings_ui` (6 cases incl. new graphics/input edits, pages
  rendered to BMP and looked at: Input, Quality); `editor_help_audit.py --check` clean;
  `-fsyntax-only` of every touched editor/game file (only SDL-stub gaps remain).
- **Not verified:** full CMake build, a real editor run, Metal, the graphics pages' real rows
  (he_tests stubs `DrawEngineSettings`, see `tests/WidgetDesignerLinkStubs.cpp`), GameApplication
  deadzone path at run time, an export round trip.
- **Decisions taken:** catalogue categories kept (44 help keys depend on them); Window & Backend
  stays on the old `renderDefaults` block; no migration — old projects have `enabled=false`.
- **Pending:** run the editor and click through Preferences ▸ Preview ▸ Viewport Quality with a
  project that sets its graphics; check the lock rows and the shutdown restore.
- **Traps:** `EditorSettingsPanel.cpp` is not in he_tests; anything the Project Settings tab
  calls from it needs a stub in `tests/WidgetDesignerLinkStubs.cpp`.
- **Next steps:** 1. manual pass in the editor; 2. a test for `syncGraphicsSource` if it can be
  made AppContext-free.
