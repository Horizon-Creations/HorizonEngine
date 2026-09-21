#!/usr/bin/env python3
"""editor_menu_parity — is every row of the OLD menu bar still in the NEW one?

Thema 73 (docs/editor-menu-restructure-2026-09-21.md) regrouped the editor's
main menu from six menus into nine. The promise was "every function keeps a
place in the bar, on both menu paths" — the ImGui bar Windows/Linux draw
(EditorUI.cpp, BeginMainMenuBar block) and the native NSMenu macOS installs
instead (MacMenuBar.mm, install()). On a Mac the ImGui bar is dead code, so a
person clicking through the editor here never sees it; this script is the one
check both paths get from one machine.

Three questions, answered from the source itself:

  1. Is every row of the old bar (docs/editor-menu-audit-2026-09-21.md §2,
     copied below verbatim) still a row of the new ImGui bar?
  2. …and of the new native Mac bar?
  3. Where do the two new bars differ from each other, beyond the platform
     conventions listed in PLATFORM_ONLY?

Both bars are read structurally: BeginMenu/EndMenu nesting for ImGui,
`heAddSubmenu` / `initWithTitle:` / `heAddItem` / `heAddToggle` for the Mac.
Rows that both bars build from a table (entity presets, view modes, show flags,
camera presets) are expanded from the same source tables the editor reads, so
a row added to the table shows up here without touching this file.

    scripts/editor_menu_parity.py            print both trees + the report, exit 1 on a gap
    scripts/editor_menu_parity.py --quiet    report only
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ED = REPO / "src" / "HE_Editor"

# ── 1. The OLD bar, row by row (audit §2, commit 54bf9032) ───────────────────
# (menu, label). "Ground Grid" moved from a bare View row into View ▸ Show, so
# it is matched anywhere under View; everything else must be a direct row of
# SOME top-level menu (the new menu is reported, not prescribed).
OLD_ROWS = [
    ("File", "New Project"), ("File", "Open Project"), ("File", "Close Project"),
    ("File", "New Scene"), ("File", "Open Scene..."), ("File", "Add Scene Additive..."),
    ("File", "Save"), ("File", "Save All"), ("File", "Save Scene As..."), ("File", "Exit"),
    ("Edit", "Undo"), ("Edit", "Redo"),
    ("Edit", "Cut"), ("Edit", "Copy"), ("Edit", "Paste"), ("Edit", "Duplicate"), ("Edit", "Delete"),
    ("Edit", "Project Settings"), ("Edit", "Preferences"),
    ("View", "Toggle Fullscreen"), ("View", "Reset Layout"),
    ("View", "Performance Profiler"), ("View", "Environment"), ("View", "Collaboration"),
    ("View", "Source Control"), ("View", "Console"), ("View", "Audio Mixer"),
    ("View", "Undo History"), ("View", "Watch"), ("View", "Ground Grid"),
    ("View", "Scene 2"), ("View", "Scene 3"), ("View", "Scene 4"),
    ("View", "Level Script"), ("View", "Game Instance"),
    ("Assets", "Import Asset..."), ("Assets", "Refresh Assets"),
    ("Assets", "Publish Engine Content to Server..."), ("Assets", "Rebuild Manifest from Server..."),
    ("Build", "Export Project..."), ("Build", "Build and Reload Game Logic"),
    ("Help", "Documentation"), ("Help", "Search the Documentation..."),
    ("Help", "Documentation (Website)"), ("Help", "Interactive Tutorial"),
    ("Help", "Report Issue..."), ("Help", "About"),
]

# Rows that the audit lists as reachable ONLY from a toolbar popup or a context
# menu; the restructure promised them a menu address. Checked the same way.
PROMISED_ROWS = [
    ("Play", "Play"), ("Play", "Pause"), ("Play", "Step Frame"), ("Play", "Step Node"),
    ("Entity", "Create"), ("Entity", "Focus Selected"), ("Entity", "Snap to Ground"),
    ("Entity", "Hide Selected"), ("Entity", "Isolate Selected"), ("Entity", "Show All"),
    ("Entity", "Group"), ("Entity", "Ungroup"), ("Entity", "Lock"), ("Entity", "Save as Prefab"),
    ("File", "Recent Projects"), ("File", "Import Asset..."),
    ("Assets", "Create Asset..."), ("Window", "Landscape Tools"),
    ("View", "View Mode"), ("View", "Show"), ("View", "Camera"),
]

# Differences between the two new bars that are platform convention, not a gap.
# Each entry: (side, path) — the side that has it, and why it is fine.
PLATFORM_ONLY = {
    # macOS puts these in the application menu / uses the system verbs.
    ("mac", "HorizonEditor"): "app menu (About, Preferences, Hide, Quit)",
    ("imgui", "File/Exit"): "macOS: HorizonEditor ▸ Quit HorizonEditor (⌘Q)",
    ("imgui", "Edit/Preferences"): "macOS: HorizonEditor ▸ Preferences… (⌘,)",
    ("imgui", "Help/About"): "macOS: HorizonEditor ▸ About Horizon Editor",
    ("imgui", "View/Toggle Fullscreen"): "macOS: View ▸ Toggle Full Screen (native, ⌃⌘F)",
    ("mac", "View/Toggle Full Screen"): "native full-screen verb",
    ("mac", "Window/Minimize"): "standard Mac Window menu",
    ("mac", "Window/Zoom"): "standard Mac Window menu",
    # ImGui shows the ternary's alternatives as one row; the Mac retitles the
    # item per frame (setItemTitle) — the script keeps the first alternative.
    # Camera bookmarks: the ImGui submenu reuses the toolbar's popup (with
    # Bookmarks 1-9 + Set/Clear); the Mac submenu carries presets + Orthographic
    # only. Bookmarks stay reachable from the toolbar's View popup on both.
    ("imgui", "View/Camera/Bookmarks"): "Mac Camera submenu has no bookmark rows (toolbar popup has them)",
    ("imgui", "View/Camera/Set Bookmark"): "see Bookmarks",
    ("imgui", "View/Camera/Clear Bookmarks"): "see Bookmarks",
}


def norm(label: str) -> str:
    """'Open Scene…' and 'Open Scene...' are the same row; so are the
    'Cmd+'-style trailing key hints ImGui puts in a second column."""
    return label.replace("…", "...").strip().rstrip(".")


# ── 2. Source tables both bars expand ────────────────────────────────────────
def entity_presets() -> list[str]:
    src = (ED / "OutlinerPanel.cpp").read_text(encoding="utf-8")
    m = re.search(r"kPresetTable\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    rows = re.findall(r'\{\s*\{\s*"([^"]+)",\s*"([^"]*)"\s*\}', m.group(1))
    return [f"{g}/{l}" if g else l for l, g in rows]


def view_modes() -> list[str]:
    src = (REPO / "src/HE_Core/include/Renderer/IRenderer.h").read_text(encoding="utf-8")
    m = re.search(r"viewModeName\(.*?\)\s*\{(.*?)\n\}", src, re.S)
    names = re.findall(r'return\s+"([^"]+)"', m.group(1))
    out: list[str] = []
    for n in names:
        if n not in out:
            out.append(n)
    return out


def show_flags() -> list[str]:
    src = (ED / "ViewportPanel.cpp").read_text(encoding="utf-8")
    m = re.search(r"showFlagFields\(int&.*?\)\s*\{(.*?)\n\}", src, re.S)
    return re.findall(r'"Viewport[A-Za-z]+",\s*&ShowFlags::\w+,\s*"([^"]+)"', m.group(1))


def camera_presets() -> list[str]:
    src = (ED / "ViewportToolbar.cpp").read_text(encoding="utf-8")
    return re.findall(r'\{\s*VP::\w+,\s*"([^"]+)",\s*"[^"]*"\s*\}', src)


# ── 3. The NEW ImGui bar (EditorUI.cpp) ──────────────────────────────────────
STRING = r'"((?:[^"\\]|\\.)*)"'


def imgui_tree() -> dict[str, list[str]]:
    """{top menu: [row path relative to it]} from the BeginMainMenuBar block."""
    src = (ED / "EditorUI.cpp").read_text(encoding="utf-8")
    start = src.index("ImGui::BeginMainMenuBar();")
    end = src.index("ImGui::EndMainMenuBar();", start)
    block = src[start:end]

    tree: dict[str, list[str]] = {}
    stack: list[str] = []

    def add(label: str) -> None:
        if not stack:
            return
        top = stack[0]
        rel = "/".join(stack[1:] + [label]) if len(stack) > 1 else label
        tree.setdefault(top, []).append(norm(rel))

    def add_many(labels: list[str]) -> None:
        for l in labels:
            add(l)

    def first_arg(pos: int) -> str:
        """The first argument of a call whose '(' is at `pos`, read with
        string and paren awareness (ternaries carry parentheses)."""
        depth, i, in_str = 0, pos, False
        while i < len(block):
            c = block[i]
            if in_str:
                if c == "\\":
                    i += 1
                elif c == '"':
                    in_str = False
            elif c == '"':
                in_str = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    return block[pos + 1:i]
            elif c == "," and depth == 1:
                return block[pos + 1:i]
            i += 1
        return block[pos + 1:]

    token = re.compile(
        r"ImGui::BeginMenu\(\s*" + STRING
        + r"|ImGui::EndMenu\(\)"
        + r"|EditorWidgets::menuItem(?=\()"
        + r"|ImGui::MenuItem(?=\()"
        + r"|OutlinerPanel::drawCreateEntityMenu\("
        + r"|ViewportToolbar::viewModeRows\("
        + r"|ViewportToolbar::showRows\("
        + r"|ViewportToolbar::viewPopup\(",
        re.S,
    )
    for m in token.finditer(block):
        t = m.group(0)
        if t.startswith("ImGui::BeginMenu"):
            label = m.group(1)
            if len(stack) == 0:
                tree.setdefault(label, [])
            else:
                add(label)
            stack.append(label)
        elif t == "ImGui::EndMenu()":
            stack.pop()
        elif t.startswith("EditorWidgets::menuItem") or t.startswith("ImGui::MenuItem"):
            arg = first_arg(m.end()).strip()
            lits = re.findall(STRING, arg)
            if lits:
                # A ternary ("Play"/"Stop"/…) is one row whose title changes;
                # every alternative counts as present.
                add_many(lits)
            elif arg.endswith(".c_str()") or arg.startswith("name"):
                add("<dynamic: " + arg.split(".")[0] + ">")
        elif "drawCreateEntityMenu" in t:
            add_many(entity_presets())
        elif "viewModeRows" in t:
            add_many(view_modes())
        elif "showRows" in t:
            add_many(show_flags() + ["Show All Overlays", "Hide All Overlays"])
        elif "viewPopup" in t:
            add_many(camera_presets() + ["Orthographic", "Bookmarks", "Set Bookmark", "Clear Bookmarks"])
    return tree


# ── 4. The NEW native Mac bar (MacMenuBar.mm) ────────────────────────────────
def mac_tree() -> dict[str, list[str]]:
    src = (ED / "MacMenuBar.mm").read_text(encoding="utf-8")
    start = src.index("void install()")
    end = src.index("NSApp.mainMenu = main;", start)
    block = src[start:end]

    # variable name → full path of the NSMenu it holds
    menus: dict[str, str] = {}
    tree: dict[str, list[str]] = {}
    # holder items: NSMenuItem* X = [[NSMenuItem alloc] initWithTitle:@"T" ...]
    # followed by NSMenu* Y = [[NSMenu alloc] initWithTitle:@"T"] and
    # [PARENT addItem:X]. Resolve the submenu's parent through the addItem line.
    holder_title: dict[str, str] = {}

    def put(var: str, label: str) -> None:
        path = menus.get(var)
        if path is None:
            return
        top, _, rel = path.partition("/")
        tree.setdefault(top, []).append(norm(f"{rel}/{label}" if rel else label))

    pending_holder: str | None = None
    for line in block.splitlines():
        s = line.strip()
        m = re.match(r"NSMenu\*\s+(\w+)\s*=\s*heAddSubmenu\(main,\s*@" + STRING, s)
        if m:
            menus[m.group(1)] = m.group(2)
            tree.setdefault(m.group(2), [])
            continue
        m = re.match(r"NSMenuItem\*\s+(\w+)\s*=\s*\[\[NSMenuItem alloc\]\s*$", s) or \
            re.match(r"NSMenuItem\*\s+(\w+)\s*=\s*\[\[NSMenuItem alloc\]\s*initWithTitle:@" + STRING, s)
        if m and m.lastindex == 2:
            holder_title[m.group(1)] = m.group(2)
            continue
        if m:
            pending_holder = m.group(1)
            continue
        m = re.match(r"initWithTitle:@" + STRING, s)
        if m and "pending_holder" in locals() and pending_holder:
            holder_title[pending_holder] = m.group(1)
            # a bare NSMenuItem with an action is a row of the menu it is added to
            pending_item = (pending_holder, m.group(1))
            pending_holder = None
            continue
        m = re.match(r"NSMenu\*\s+(\w+)\s*=\s*\[\[NSMenu alloc\]\s*initWithTitle:(?:@" + STRING + r"|(\w+)\.title)\]", s)
        if m:
            title = m.group(2) or holder_title.get(m.group(3), "?")
            menus[m.group(1)] = title  # parent fixed up on the addItem line
            continue
        m = re.match(r"\[(\w+)\s+addItem:(\w+)\];", s)
        if m:
            parent, item = m.group(1), m.group(2)
            if parent == "main":
                # a top-level menu built by hand (Entity): its NSMenu becomes a root
                title = holder_title.get(item)
                if title:
                    tree.setdefault(title, [])
                    for var, path in list(menus.items()):
                        if path == title:
                            menus[var] = title
                continue
            if parent not in menus:
                continue
            # a submenu holder: retarget the child menu under its parent
            for var, path in list(menus.items()):
                if var != parent and "/" not in path and path == holder_title.get(item) and var not in ("main",):
                    menus[var] = menus[parent] + "/" + path
            title = holder_title.get(item)
            if title:
                put(parent, title)
            continue
        m = re.match(r"(?:s_\w+\.push_back\(\s*)?(?:heAddItem|heAddToggle)\(\s*(\w+)\s*,\s*@" + STRING, s) or \
            re.match(r"(?:heAddItem|heAddToggle)\(\s*(\w+)\s*,\s*@" + STRING, s)
        if m:
            put(m.group(1), m.group(2))
            continue
        m = re.match(r"heAddItem\(\s*(?:group \? group : )?(\w+),\s*\[NSString stringWithUTF8String:rows\[i\]\.label\]", s)
        if m:
            for p in entity_presets():
                put(m.group(1), p)
            continue
        m = re.match(r"heAddToggle\((\w+),\s*\[NSString stringWithUTF8String:HE::viewModeName", s)
        if m:
            for v in view_modes():
                put(m.group(1), v)
            continue
        m = re.match(r"heAddToggle\((\w+),\s*\[NSString stringWithUTF8String:fields\[i\]\.label\]", s)
        if m:
            for f in show_flags():
                put(m.group(1), f)
            continue
        m = re.match(r"heAddToggle\((\w+),\s*\[NSString stringWithUTF8String:EditorCamera::presetName", s)
        if m:
            for c in camera_presets():
                put(m.group(1), c)
            continue
        m = re.match(r"\{\s*C::\w+,\s*@" + STRING + r"\s*\}", s)
        if m:  # the Scene 2/3/4 panes table, added to `window` in a loop
            put("window", m.group(1))
            continue
    # The Recent Projects submenu is filled per frame (setRecentProjects);
    # its holder row is what the bar shows. Likewise three rows change their
    # title with the editor's state (setItemTitle in EditorUI.cpp's Mac
    # dispatch): every title such a call can produce counts as present, next
    # to the row it retitles.
    ui = (ED / "EditorUI.cpp").read_text(encoding="utf-8")
    cmd_row = {"PlayToggle": ("Play", "Play"), "PauseToggle": ("Play", "Pause"),
               "ToggleLock": ("Entity", "Lock")}
    for m in re.finditer(r"MacMenuBar::setItemTitle\(MC::(\w+),(.*?)\);", ui, re.S):
        top, base = cmd_row.get(m.group(1), (None, None))
        if top is None:
            continue
        rows = tree.setdefault(top, [])
        for title in re.findall(STRING, m.group(2)):
            if norm(title) not in rows:
                rows.insert(rows.index(base) + 1 if base in rows else len(rows), norm(title))
    return tree


# ── 5. Report ────────────────────────────────────────────────────────────────
def find_row(tree: dict[str, list[str]], label: str, prefer: str | None = None) -> str | None:
    """Where a label lives: 'Menu/…/label', direct rows first."""
    want = norm(label)
    hits: list[str] = []
    for top, rows in tree.items():
        for r in rows:
            last = r.rsplit("/", 1)[-1]
            if last == want:
                hits.append(f"{top}/{r}")
    if not hits:
        return None
    hits.sort(key=lambda p: (0 if prefer and p.startswith(prefer + "/") else 1, p.count("/")))
    return hits[0]


def main(argv: list[str]) -> int:
    quiet = "--quiet" in argv
    im = imgui_tree()
    mac = mac_tree()

    if not quiet:
        for name, tree in (("ImGui bar (EditorUI.cpp)", im), ("native Mac bar (MacMenuBar.mm)", mac)):
            print(f"== {name}: {' | '.join(tree)}")
            for top, rows in tree.items():
                print(f"  {top}")
                for r in rows:
                    print(f"    {r}")
        print()

    gaps = 0
    print("== old row -> new place (ImGui | Mac)")
    for menu, label in OLD_ROWS + [("*", "-")] + PROMISED_ROWS:
        if menu == "*":
            print("-- promised by the restructure (were toolbar/context-menu only):")
            continue
        a = find_row(im, label, prefer=menu)
        b = find_row(mac, label, prefer=menu)
        if a is None and ("imgui", f"{menu}/{label}") not in PLATFORM_ONLY:
            pass
        ok_a = a is not None or any(k[0] == "mac" for k in PLATFORM_ONLY)  # placeholder, refined below
        a_txt = a or "MISSING"
        b_txt = b or "MISSING"
        # a Mac miss is fine only if the row lives in the app menu / is a
        # native verb (PLATFORM_ONLY names the ImGui path).
        mac_ok = b is not None or ("imgui", f"{menu}/{label}") in PLATFORM_ONLY \
            or ("imgui", a or "") in PLATFORM_ONLY
        if b is None and mac_ok:
            b_txt = "app menu / native verb"
        flag = "" if (a is not None and mac_ok) else "   <-- GAP"
        if flag:
            gaps += 1
        print(f"  {menu + '/' + label:42s} -> {a_txt:40s} | {b_txt}{flag}")

    print()
    print("== ImGui vs Mac (rows only one side has, beyond platform convention)")
    im_paths = {f"{t}/{r}" for t, rows in im.items() for r in rows}
    mac_paths = {f"{t}/{r}" for t, rows in mac.items() for r in rows}
    im_paths = {p for p in im_paths if not p.split("/")[-1].startswith("<dynamic")}
    diffs = 0
    for p in sorted(im_paths - mac_paths):
        why = PLATFORM_ONLY.get(("imgui", p))
        print(f"  ImGui only: {p:45s} {'(' + why + ')' if why else '<-- DIFF'}")
        diffs += 0 if why else 1
    for p in sorted(mac_paths - im_paths):
        top = p.split("/")[0]
        why = PLATFORM_ONLY.get(("mac", p)) or PLATFORM_ONLY.get(("mac", top))
        print(f"  Mac only:   {p:45s} {'(' + why + ')' if why else '<-- DIFF'}")
        diffs += 0 if why else 1

    order_im = list(im)
    order_mac = [t for t in mac if t != "HorizonEditor"]
    print()
    print(f"== top-level order  ImGui: {order_im}")
    print(f"                    Mac:   {order_mac}")
    same_order = order_im == order_mac
    print(f"== result: {gaps} gap(s) old->new, {diffs} unexplained ImGui/Mac difference(s), "
          f"order {'identical' if same_order else 'DIFFERS'}")
    return 0 if (gaps == 0 and diffs == 0 and same_order) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
