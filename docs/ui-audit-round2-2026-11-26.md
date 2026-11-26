# UI audit, second round (2026-11-26)

The first round (`docs/ui-audit-fixes-2026-11-25.md`) looked at about 60 of the
headless UI shots. This round looked at the other ~280 (team and Git dialogs,
Planner and docs, widget designer and graphs, Inspector, landscape, settings) and
fixed what a user would notice: one name per thing, one control style per kind,
labels on the left, no duplicate hints, states that are visible.

## Handoff

**Done (release/0.8.0, one commit)**

- **Shared widgets.**
  - `EditorWidgets::helpMarker` draws a circled "?" instead of the text "(?)" (`EditorRowWidgets.cpp`).
  - `Row::colorEdit3` shows a mixed selection as a swatch with "—".
  - `AudioMix::drawBusCombo` takes `emptyLabel`. The Inspector bus rows pass "The Clip's Bus", because for Weather and Audio Source "" means the clip's own bus.
  - The mono font merges the body font, so `…`, `—` and the like stop showing as `?` (`EditorApplication.cpp`, font setup).
- **Widget designer** (`UIEditorPanel.cpp`).
  - The Events section lists only the added events, plus one "Add Event..." combo.
  - Interaction labels are Title Case. Their help keys were renamed with them.
  - The palette labels are capped at body size.
  - The Focused state preview says when it looks like Normal.
  - The hints were tidied.
- **HorizonCode** (`HcGraphHost.cpp`).
  - `spacedName` is used for display only: pins, event titles, and the drag menu's "Get/Set" and "(Control)" entries. Graphs still store the raw names.
  - Save accessors are named "Set/Get Saved Number…" (`EngineApi.cpp` display map). Codegen derives local names from them, so `tests/fixtures/hcodegen_golden/hcgen_EngineExecCached.cpp` was updated.
- **Material editor.**
  - The Output node is 52 units tall.
  - The blend-mode icon is now `iconCircle`, no longer the eye.
  - The tessellation popup keeps ImGui's label-right drags. With `Row::InlineLabels` it grew about 85 px and covered the backend badge.
- **Docs reader** (`DocsPanel.cpp`).
  - A code pill widens the gap next to it.
  - A word glued to the previous one never wraps on its own.
  - A multi-word link gets one continuous underline.
  - "• text" cells draw the tick plus the text.
- **Team / Git**:
  - New `TeamController::stopHosting()` and a "Stop Hosting" button.
  - "Open Team Settings" in the Sync view, with a stub in `tests/WidgetDesignerLinkStubs.cpp`.
  - Recovery dialogs have no empty title band; "Delete Copy".
  - The Git and LFS remedy dialogs use the same labels.
  - Tool Setup: the footer is pinned and Retry is the primary button.
  - The sync card tone and headline now count unresolved files.
- **Planner**:
  - Icon close/remove buttons.
  - Lane header "Open · 3" with an empty-lane hint.
  - "Timeline / Calendar", "Open Planner" and "All done".
  - Fit inset in screen pixels.
  - Readable history times.
  - The comment box sits above the history.
- **Inspector / landscape / settings**:
  - Labels on the left for the remaining checkboxes: water, setup, spline, permissions.
  - Integer fields are centred drags.
  - Material parameters: captions ellipsized, "reserved" hidden, no RGB tint on plain vectors.
  - Replication is drawn last.
  - Spline points read "Point N", with Remove on the same row.
  - Streaming labels and pluralisation fixed.
  - Brush caption shows the real diameter.
  - Destructive buttons are red.
  - Colour is spelled "color" everywhere in the UI.
- **Help.** All renamed and new keys are in `EditorHelp.cpp`. The audit's ignore entry for Add Component now names the Replication scope.

**Verified**

- Full build of HorizonEditor and he_tests.
- Full `ctest`. The first run had 4 failures, all from renamed labels or the codegen golden; after fixing them, those 4 pass.
- `editor_help_audit.py --check` is green.
- The fresh shots were compared with the old ones, about 80 pairs.
  - The regressions this turned up were fixed:
    - the clash text in Shortcuts;
    - "follow it" running off the edge;
    - docs pills with too large a gap;
    - the Armed line not lined up;
    - the brush caption;
    - the spline axis bars, now `Row::axisDrag`.
  - The shots of these fixes were checked again by eye: shortcuts-clash, inspector-spline and docs-horizoncode-recipes.
  - After that, `ctest` passed 387/387.
- At 1400 px the Planner timeline toolbar breaks onto a second row ("Calendar" is wider than "Month"). That is its normal wrap behaviour, so it was left alone.

**Not verified**

- The live editor on a real display.
- Windows CI.
- How the merged mono font looks in the script code editor; the harness uses the body font for code.

**Decisions**

- The docs bundle was NOT regenerated. The local `Website/HorizonEngineDocs` lacks the `systems#audio-*` sections the committed bundle has. Regenerating would drop them, and `test_editor_help` catches that.
- The prose "\»" in the docs stays, because it names the listing notation.
- `pinDialogToEditorWindow` now pins the Team Sync conflict window too, so it can no longer be dragged.

**Next steps**

1. Material editor: constants all named "Color" with no node name. In nodes, an asset drop slot is drawn as bare text (`EditorWidgets::assetDropSlot` inside the node child window).
2. Docs search: clear the nav highlight while a query is active, and highlight the term in the snippets.
3. Editor reference: one eyebrow per group, not one per entry (`EditorReference.cpp`).
4. Code tokens in hints still wrap after "." (`config.` / `json`). `hint()` needs a non-breaking form.
5. Website content: "((added))" in `horizoncode.html:756`.
