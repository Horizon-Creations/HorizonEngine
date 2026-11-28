# UI audit fixes (2026-11-25)

The audit looked at about 60 of the headless UI shots, out of roughly 300.
It listed 15 points and four smaller notes. This step fixes all of them.

## Handoff

**Done (commit on release/0.8.0)**

- **Sliders, editor-wide.** `HE-PATCH(slider-fill)` in `vendor/imgui/imgui_widgets.cpp` (`SliderBehaviorT`, `SliderScalar`).
  - A slider now shows a faint fill up to its value plus a 3 px marker, instead of a solid grab block.
  - Integer sliders no longer get a grab one unit wide.
  - Carry this patch along on every ImGui upgrade.
- **Label column.**
  - `Row::InlineLabels` is now active in:
    - `ProjectSettingsPanel::drawPage`;
    - the widget-designer details pane (`UIEditorPanel.cpp`, `##uiw_details`);
    - the spline lake section (`SplineTool.cpp`).
  - `TerrainTools::rowLabel` uses `Row::labelCell`.
  - In the widget designer, `detailLabel` and `drawThemeRoleButton` handle the column. In that layout the role button sits at the right end of the control's line, and `detailControlWidth()` narrows the control to make room.
  - Colour rows stay stacked (`Row::StackedLabels`) because four channel fields need the full width.
- **Field widths.**
  - `Row::dragFloat`, `dragInt` and `inputInt` are capped at 200 px (`kNumberMaxWidth`).
  - `hint()` wraps at no more than 680 px.
  - Hard `\n` line breaks in Project Settings hints are gone.
- **Buttons.**
  - New `EditorWidgets::removeButton`: red text on a red wash, at full button height. It is used for in-panel Remove/Delete.
  - `dangerButton` (filled red) is kept for confirmations inside dialogs.
  - The Git remedy dialog and the team Sign Out row use full-height buttons.
- **Bugs.**
  - The font has no ▸, → or ← glyphs (`Roboto_Condensed-Bold` cmap checked). The affected UI strings now use `»`, or reworded text.
  - "Low-res clouds" label shortened (help key renamed too).
  - Mesh "tris" count moves to its own line when the row is full.
  - The static mesh toolbar group is measured with `labelGroupWidth`.
  - Animation bar: drawn glyph buttons, with their ImGui ids still `|<`, `Play`/`Stop`, `>|` and `x`. The bar wraps instead of overflowing (`timelineSameLine`).
- **Smaller points.**
  - Collision-layer names are shown in two columns.
  - The material parameters in the Details panel show the graph's tooltip and give each vector component a caption (`componentName` parses `x = …`). Colours are shown in 0–255.
  - Material editor: names are drawn frameless, and `..`/`(?)` are glyphs.
  - The tutorial spotlight outline is amber.
  - Footer avatars are 1.2× line height, with step 0.62 (`test_team_ui` requires the stack to be narrower than 3 faces).
  - The planner row's Open button is an icon (help entry `Planner/Open Row`).
  - Hub template names shrink to 78 % before they get an ellipsis.
  - Docs tables draw a lone `•` cell as an amber tick.

**Verified**

- Full `ctest` run, plus each failing test rerun on its own. The only failures were `test_collab_controller` and `test_jobsystem`: both failed only under `-j 8` load and passed 3 of 3 times on their own.
- `editor_help_audit.py --check` is green.
- The shots were checked by eye: inspector-sky and inspector-water, timeline, project-settings-*, static-mesh, foliage, material editor, docs-tables and widget-designer-image-details.

**Not verified**

- The live editor on a real display.
- Windows CI.
- The audit frames it did not look at.
- Whether the label column on the Project Settings pages fits the Windows font metrics.

**Next steps**

- A docs table cell that starts with `• ` followed by text ("• (basic)") still shows a bullet.
- The mono font (`AddFontDefaultVector`) has no `…`, which shows as `?` in export logs.
