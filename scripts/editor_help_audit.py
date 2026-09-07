#!/usr/bin/env python3
"""editor_help_audit — which editor controls explain themselves, and which do not.

The editor's manual is meant to answer one question well: "what does THIS
control do?" A tooltip that is missing is invisible — nothing fails to build,
nothing fails a test, the user simply hovers and gets nothing. So the gap is
measured here instead, by walking the panels' source for labelled widgets and
holding them against the keys in src/HE_Editor/EditorHelp.cpp.

    scripts/editor_help_audit.py            # the table, by area
    scripts/editor_help_audit.py --list AREA  # the open ones in that area
    scripts/editor_help_audit.py --check    # exit 1 if coverage dropped

--check is the regression guard, wired into ctest as `editor_help_audit`: it
fails when a NEW uncovered control appears, using the counts recorded in
BASELINE below. Raise the numbers there in the same commit that covers an area
— that is the whole ritual.

It reads source, not the binary, so it is approximate by construction: a label
built at run time cannot be seen here, and a widget behind a macro will be
missed. It is a floor on the gap, never a ceiling.
"""

from __future__ import annotations

import argparse
import collections
import os
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src" / "HE_Editor"

# Widgets that put a label on screen the user can hover.
VALUE = (r"(?:EditorWidgets::Row::\w+|Row::\w+|EditorWidgets::checkbox|ImGui::Checkbox"
         r"|ImGui::SliderFloat|ImGui::SliderInt|ImGui::DragFloat\d?|ImGui::DragInt\d?"
         r"|ImGui::InputText|ImGui::InputInt|ImGui::InputFloat|ImGui::Combo"
         # BeginCombo is a labelled widget like any other, and leaving it out is
         # how eight pickers in the HorizonCode graphs went eight rounds without
         # anyone noticing — the scan simply never asked about them.
         r"|ImGui::BeginCombo|ImGui::RadioButton"
         r"|ImGui::ColorEdit\d)")
ACTION = (r"(?:ImGui::Button|ImGui::SmallButton|ImGui::MenuItem|ImGui::BeginMenu"
          r"|ImGui::Selectable"
          r"|EditorWidgets::menuItem|EditorWidgets::button|EditorWidgets::selectable"
          r"|EditorWidgets::smallButton"
          r"|EditorWidgets::primaryButton|EditorWidgets::dangerButton"
          r"|EditorWidgets::dangerMenuItem|EditorWidgets::dangerSmallButton)")

# Buttons whose wording IS the explanation. Counting these would inflate the gap
# with work nobody should do: "Cancel" does not need a paragraph.
CHROME = {
    "OK", "Cancel", "Close", "Save", "Apply", "Done", "Yes", "No", "Browse",
    "Remove", "Copy", "Paste", "Cut", "Undo", "Redo", "Delete", "Rename",
    "Refresh", "Back", "Next", "Open", "Create", "+", "-", "x",
    "\u00d7", "\\xc3\\x97", "\\xC3\\x97",   # the clear-a-slot glyph, as text and as escapes
    "(none)",          # a combo's empty placeholder, not a control
}

# Controls the scan sees but that are covered another way, or are not controls.
# Listed with the reason, because an ignore list without one is a place for
# things to hide.
IGNORE = {
    # Covered by an explicit helpForKey("details.add-component") call — the scan
    # matches labels against scopes and cannot see a key passed by hand.
    ("UI Button", "Add Component"),
    # The documentation reader pushes its scope in draw(), which is at the END
    # of the file, while these buttons are drawn by helpers defined above it.
    # At run time the scope is open before the helper is called; a scan that
    # walks a file top to bottom cannot see that. Covered by the runtime lookup
    # test in tests/test_editor_help.cpp instead.
    (None, "Show me"), (None, "Start"), (None, "Online"),
    ("Documentation", "Open the manual online"),
    # The one literal entry in the Target Class dropdown (the others are asset
    # names, built at run time and invisible here). It is not a control with an
    # explanation of its own: "HorizonCode Node/Target Class" is the entry, and it
    # explains "From the wire" by name.
    ("HorizonCode Node", "From the wire"),
}

# Top-level menu titles. A menu opens when you touch it, which is the whole
# explanation; an entry would be a tooltip fighting with the menu it describes.
MENU_TITLES = {"File", "Edit", "View", "Assets", "Build", "Help", "Window"}

COMPONENT_HEADER = re.compile(r'componentHeader\("([^"]+)"')
# A panel says which section its controls belong to by pushing a scope; the
# lookup at run time is "<scope>/<label>", so the scan has to follow the same
# thing. Without this the wrapped menus would look like controls that vanished
# rather than controls that got covered.
HELP_SCOPE = re.compile(r'Help::Scope\s+\w+\("([^"]*)"\)')

# The settings catalog does not push a scope per section — it retargets the open
# one at each row's CATEGORY, so a setting is keyed
# "Preferences/<category>/<label>" and the reference page can be split the way
# the window is. The category lives in the row() call and nowhere else, which is
# what keeps the two from drifting; this is the scan reading the same argument.
SETTINGS_ROW = re.compile(r'row\("[^"]*",\s*"([^"]+)"')

AREAS: dict[str, list[str]] = {
    "interface": ["EditorUI.cpp", "ViewportToolbar.cpp", "OutlinerPanel.cpp",
                  "ContentBrowserPanel.cpp", "ProjectHubPanel.cpp", "ConsolePanel.cpp",
                  "NotificationBar.cpp", "PlayReportPanel.cpp", "DocsPanel.cpp",
                  "TutorialPanel.cpp"],
    "components": ["InspectorPanel.cpp"],
    "settings": ["EditorSettingsPanel.cpp", "ToolchainDialog.cpp"],
    "materials": ["MaterialEditorPanel.cpp"],
    "ui": ["UIEditorPanel.cpp", "ThemeAssetPanel.cpp"],
    "horizoncode": ["LevelScriptPanel.cpp", "HcGraphHost.cpp", "HcEditorUtil.cpp",
                    "TypeAssetPanel.cpp"],
    "input": ["InputAssetPanel.cpp"],
    "animation": ["AnimatorStateMachineEditorPanel.cpp", "AudioEditorPanel.cpp",
                  "StaticMeshEditorPanel.cpp", "SkeletalMeshEditorPanel.cpp"],
    "landscape": ["TerrainTools.cpp", "EnvironmentPanel.cpp"],
    "export": ["ExportDialogPanel.cpp", "BuildProgressDialog.cpp", "ProfilerPanel.cpp"],
    "collab": ["CollabPanel.cpp", "CollabPresenceBar.cpp", "SourceControlPanel.cpp",
               "GitMissingDialog.cpp", "EngineContentPublishDialog.cpp",
               "ReportIssueDialog.cpp"],
}

# The gap as it stands, per area. A number that goes UP is a control somebody
# added without an entry; --check is what says so. Lower them as areas are
# covered — never raise one to make the check pass.
BASELINE = {
    "interface": 0, "components": 0, "settings": 0, "materials": 0, "ui": 0,
    "horizoncode": 0, "input": 0, "animation": 0, "landscape": 0, "export": 0,
    "collab": 0,
}


def help_keys() -> set[str]:
    text = (SRC / "EditorHelp.cpp").read_text(encoding="utf-8")
    return {m.group(1) for m in re.finditer(r'\{\s*"([^"]+)",\s*"', text)}


def scan(filename: str, keys: set[str]) -> tuple[list, list]:
    """(covered, open) labels of one panel, as (scope, label) pairs."""
    path = SRC / filename
    if not path.exists():
        return [], []
    text = path.read_text(encoding="utf-8")
    # The Details panel names its section per component; Preferences is one scope.
    scope = "Preferences" if filename == "EditorSettingsPanel.cpp" else None
    covered, missing, seen = [], [], set()
    for line in text.split("\n"):
        m = COMPONENT_HEADER.search(line)
        if m:
            scope = m.group(1)
        m = HELP_SCOPE.search(line)
        if m and m.group(1):
            scope = m.group(1)
        if filename == "EditorSettingsPanel.cpp":
            m = SETTINGS_ROW.search(line)
            if m:
                scope = "Preferences/" + m.group(1)
        for pattern, is_action in ((VALUE, False), (ACTION, True)):
            for hit in re.finditer(pattern + r'\(\s*"([^"]+)"', line):
                raw = hit.group(1)
                if raw.startswith("##") or not raw.strip():
                    continue
                visible = raw.split("##")[0]
                if is_action and visible.strip(". ") in CHROME:
                    continue
                if (scope, visible) in seen or (scope, visible) in IGNORE:
                    continue
                if visible in MENU_TITLES and "BeginMenu" in hit.group(0):
                    continue
                seen.add((scope, visible))
                candidates = [raw, visible, f"Component/{visible}"]
                if scope:
                    candidates += [f"{scope}/{raw}", f"{scope}/{visible}"]
                (covered if any(c in keys for c in candidates) else missing).append(
                    (scope, visible))
    return covered, missing


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", metavar="AREA", help="print the open controls of one area")
    ap.add_argument("--check", action="store_true",
                    help="fail when an area has more open controls than the baseline")
    args = ap.parse_args()

    keys = help_keys()
    result = {}
    for area, files in AREAS.items():
        cov, miss = [], []
        for f in files:
            c, m = scan(f, keys)
            cov += c
            miss += m
        result[area] = (cov, miss)

    if args.list:
        cov, miss = result.get(args.list, ([], []))
        if not miss:
            print(f"{args.list}: nothing open")
        for scope, label in miss:
            print(f"  [{scope or '-'}] {label}")
        return 0

    print(f"{'Area':14s} {'controls':>9s} {'covered':>8s} {'open':>6s} {'baseline':>9s}")
    worse = []
    for area, (cov, miss) in result.items():
        base = BASELINE.get(area)
        flag = ""
        if base is not None and len(miss) > base:
            flag = "  <-- REGRESSED"
            worse.append(area)
        print(f"{area:14s} {len(cov) + len(miss):9d} {len(cov):8d} {len(miss):6d} "
              f"{base if base is not None else '-':>9}{flag}")
    total_open = sum(len(m) for _, m in result.values())
    total_cov = sum(len(c) for c, _ in result.values())
    print(f"{'TOTAL':14s} {total_cov + total_open:9d} {total_cov:8d} {total_open:6d}")
    print(f"\nhelp entries: {len(keys)}")

    if args.check and worse:
        print(f"\nnew uncovered controls in: {', '.join(worse)}", file=sys.stderr)
        print("add a help entry for them, or lower the baseline if one was removed",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
