# Editor contrast: text on coloured grounds (topic 224, step 2)

The Tool Setup dialog showed an amber "C++ compiler was not installed." line on an
amber-brown row. Step 1 placed that row and measured the flash, but stopped before
the sweep. This step did the sweep, added contrast helpers to `EditorTheme.h` and
fixed every pair below 4.5:1 that it found.

## The rule

`Theme::kMinTextContrast = 4.5` (WCAG body text), measured on the 8-bit colour as it
lands on screen. The helpers live in `src/HE_Editor/EditorTheme.h`:

| Helper | Use |
|---|---|
| `over(top, under)` | composite a translucent fill onto its surface before measuring |
| `contrastRatio(a, b)` | the WCAG ratio, 1..21 |
| `textOn(fill, light = Text)` | label for a filled ground: `light` while it clears 4.5, otherwise `Ink` (warm 0.08) if that is better |
| `readableOn(fg, bg)` | `fg` moved toward Text (or Ink) only as far as needed; unchanged if it already clears |

The ratio measures lightness only. The reported Warn line was **4.70:1**, which passes,
and a person still called it unreadable, because amber text on amber is a hue clash.
So same-hue pairs are fixed by not drawing them (the flash moved its accent out from
under the text), not by a ratio. `tests/test_editor_theme.cpp` ("theme contrast: …",
6 cases) holds the palette's own pairs to the floor.

## Findings and fixes (WCAG ratio before → after)

| # | Where | Pair | Before | After |
|---|---|---|---|---|
| 1 | `ToolSetupDialog.cpp` drawRow, focus flash | Warn / TextDim / Text on accent 35 % over PopupBg | 4.70 / **1.88** / 7.55 | flash is Ivory 8 % + 3 px accent bar; Warn 6.48, TextDim ≥4.5 via `readableOn`, Text 10.4 |
| 2 | `EditorTheme.h` `TextDim` (every TextDisabled, dim line, placeholder) | on PopupBg / FrameBg | 3.26 / 2.94 | 0.46 → 0.60: 5.20 / 4.68 (Text:TextDim still ~2.5:1) |
| 3 | `EditorToolbar.h` `kFgDim` (bar readouts, disabled cells) | on well / bar | 2.52 / 2.87 | mirror of TextDim: 5.20 / 5.92 |
| 4 | `EditorRowWidgets.cpp` `primaryButton` (Install, Accept, …) | white on Accent / AccentHi / pressed | 3.21 / 2.40 / 4.46 | Ink: 5.68 / 7.61 / 4.84 (pressed fill mix 0.80 → 0.90) |
| 5 | `EditorTheme.cpp` ButtonActive / HeaderActive (also "toggle on" fill in Source Control, UI editor) | Text over WindowBg | 3.41 / 3.33 | accent 0.80/0.85 → 0.55: 5.60 |
| 6 | `AudioMixerPanel.cpp` lit M / S / EQ | Text on Accent | 2.63 | Ink 5.68 (hover 7.61) |
| 7 | `InputAssetPanel.cpp` armed Bind / "Press any input…" | Text on literal amber (214,122,30) / hover | 2.58 / 2.11 | `pushArmedColours()`: Accent + Ink, 5.68 / 7.61 |
| 8 | `EditorToolbar.h` armed cell hovered | kFgOn on kOnBgHover | 3.24 | hover deepens (150,89,27): 5.48 |
| 9 | `ConsoleView.cpp` error / critical rows, repeat pill, timestamps | kBad on kBadWell / crit tint / pill; TextDim on tint | 3.94 / 3.16 / 2.44 / 2.91 | all via `readableOn` on the composited ground: ≥4.5 |
| 10 | `OutlinerPanel.cpp` drawChip (collaborator lock, prefab) | colour on 22 % pill of itself; Ink on accent 0.80 | 3.33–3.49; 4.15 | ink via `readableOn` on the pill: ≥4.5; changed-prefab fill opaque: 5.68 |
| 11 | `SourceControlPanel.cpp` type-icon letter | white on Mesh / Texture / Other tile | 2.96 / 3.35 / 3.75 | `textOn(tile, white)`: 6.17 / 5.44 / 4.86 |
| 12 | `EditorRowWidgets.cpp` `dangerSmallButton` (Remove) | red label on red wash, hovered / pressed, over PopupBg | 3.05 / 3.20 | wash 0.75/1.0 → 0.32/0.50: 4.5 / 4.66 |

Measured and left alone (pass): ProjectHub red row header 10.75, Material editor error
banner 9.61, Profiler capture banner (AccentBright 9.47), HorizonCode/Material comment
header Text 9.28, UI editor playhead tag 6.42, HC search-palette highlight Text 4.62,
`dangerButton` white 5.39 to 7.30. No text on them: Collab/Team colour swatches,
Content Browser selected icon buttons.

**Residual:** the Source Control type letters for Material (4.45 with Ink) and Code
(4.36 with white) stay just under 4.5 whichever colour they get. They are one-letter
icon glyphs, which clear the 3:1 non-text threshold; changing the category hues
would be a palette decision of its own.

## Screenshots

Before/after pairs in `docs/editor-contrast-2026-10-19/`, from the headless
ImGuiSoftwareRaster tests (`HE_UI_DUMP_DIR`):

- `tool-setup-flash-failed-*.png`, `tool-setup-flash-missing-*.png`: the reported
  frame (new test "Tool setup: the row a banner points at is marked when the dialog
  opens"); rows 1, 2 and 4.
- `console-*.png`: row 9 (`ui shot: the console`).
- `audio-mixer-muted-*.png`: rows 2 and 6.

Rows 3, 5, 7, 8, 10, 11 and 12 have no headless test that draws them in that state.
They are measured (arithmetic in `test_editor_theme.cpp`), not photographed.

## Handoff

- **Topic / step:** 224, step 2 of 2
- **Branch:** `claude/editor-ui-kontrast-problem-bei-status-warn-hervorhebungen-z-` (pushed, see the hive post for the commit)
- **Done:** contrast helpers in `EditorTheme.h`, TextDim and kFgDim raised, the 12 pairs above fixed, a 6-case theme test, and a Tool Setup flash shot test.
- **Files touched:** `EditorTheme.h` (helpers, TextDim), `EditorTheme.cpp` (Button/HeaderActive), `EditorToolbar.h` (kFgDim, kOnBgHover), `EditorRowWidgets.cpp` (filledButton label, primary pressed, dangerSmallButton), `ToolSetupDialog.cpp` (drawRow flash, rowText), `ConsoleView.cpp` (tinted rows), `OutlinerPanel.cpp` (drawChip), `SourceControlPanel.cpp` (type letter), `AudioMixerPanel.cpp`, `InputAssetPanel.cpp`; tests `test_editor_theme.cpp`, `test_tool_setup_ui.cpp`.
- **Verified:** `he_tests` and `HorizonEditor` built (Release, ninja -j8); `editor_help_audit.py --check` clean. `test_editor_theme` (10 cases) and `test_tool_setup_ui` (15) pass. Full `ctest -j4`: 303/304 pass. The one failure, `test_hpak` "Disk registry: scanContentDirectory" (6 == 2), is this Mac's ~/Library/…/EngineContentCache (4 synced assets). It passes with `HOME=/tmp/x`, so it is not this change (hive lesson 475). Before/after shots inspected by eye.
- **Not verified:** the editor itself on screen (no GUI runs from a bee); Windows/Linux CI (dispatched, not waited for).
- **Decisions taken:** primary buttons get dark Ink labels on amber, a visible brand change, because white cannot reach 4.5 on that amber. TextDim globally 0.60. The flash's accent moved to an edge bar. Toolbar armed hover deepens instead of lightening.
- **Decisions suggested, not taken:** re-hue the Source Control Material/Code tiles; a selected (Header-washed) console error row was not measured.
- **Traps:** WCAG passes same-hue pairs (4.7:1 amber on amber), so don't "fix" such a pair by tuning a ratio. `EditorToolbar.h` literals mirror theme values by hand; the test checks kFgDim == TextDim. Use `/usr/bin/grep`; the wrapped grep returns nothing.
- **Next steps:** 1. The human looks at the shots and the amber-button Ink decision. 2. New coloured grounds use `Theme::textOn` / `readableOn` and add their pair to `test_editor_theme.cpp`.
