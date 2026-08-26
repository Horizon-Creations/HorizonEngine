#!/usr/bin/env python3
"""build_docs_bundle — turn the published docs into the editor's offline manual.

The manual lives on the website as hand-written HTML (Website/HorizonEngineDocs/
*.html, a sibling checkout of this repo). Help ▸ Documentation used to be a plain
SDL_OpenURL onto that site, which fails exactly when it is needed most: on a
machine without a browser at hand, offline, or when the user is mid-gesture in
the editor and does not want to leave it.

So the same text ships INSIDE the editor. This script converts every docs page
into one JSON bundle of *blocks* — paragraphs, tables, code listings, callouts —
which DocsPanel renders with ImGui and DocsLibrary searches. Blocks rather than
the flat plain text the website's own search index (docs-index.json) carries:
half of the editor manual is reference tables, and a table flattened to a text
blob is unreadable.

    scripts/build_docs_bundle.py [--docs DIR] [--out FILE] [--check]

Defaults: --docs ../Website/HorizonEngineDocs (searched next to this repo),
--out EditorDeps/Docs/he-docs.json. Figure images referenced by the pages are
copied next to the bundle.

The OUTPUT IS COMMITTED. That is deliberate: CI, a fresh clone and anyone
building the editor must not need the website checkout, and the bundle is the
one thing here that cannot be regenerated without it. Re-run this after editing
the docs (the deploy of the website is a separate step), and commit the result.

--check re-runs the conversion and exits non-zero if the committed bundle is
stale, without writing anything.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from datetime import datetime, timezone
from html.parser import HTMLParser
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def find_docs_dir() -> Path:
    """Locate the website's HorizonEngineDocs next to this checkout.

    Walks up rather than looking at exactly one level: a git WORKTREE of this
    repo lives several directories below the real one (.claude/worktrees/…), so
    "the sibling of my parent" is wrong there and the script would refuse to run
    in exactly the checkout someone is developing in.
    """
    for base in [REPO_ROOT, *REPO_ROOT.parents]:
        candidate = base.parent / "Website" / "HorizonEngineDocs"
        if candidate.is_dir():
            return candidate
    return REPO_ROOT.parent / "Website" / "HorizonEngineDocs"


DEFAULT_DOCS = find_docs_dir()
DEFAULT_OUT = REPO_ROOT / "EditorDeps" / "Docs" / "he-docs.json"

BASE_URL = "https://horizoncreations.dev/HorizonEngineDocs/"

# Bumped when the *schema* below changes in a way DocsLibrary must reject rather
# than misread. The C++ side refuses to load anything newer than it knows.
SCHEMA_VERSION = 1

# The docs home is a link grid, not prose: it has no <main class="docs-content">
# and nothing in it that is not already the group listing this bundle builds
# from the sidebar.
SKIP_PAGES = {
    "index.html",
    # The node reference is GENERATED in the editor from HE::api::registry() and
    # the node enum (src/HE_Editor/HcNodeReference.cpp) — one section per
    # callable thing, with its real pins. The website's hand-written version was
    # two sections of tables and already behind the engine, so shipping it would
    # only give the reader a worse copy under the same page id.
    "horizoncode-nodes.html",
}

TITLE_TAIL = re.compile(r"\s*[—–-]\s*Horizon Engine Documentation\s*$")


# ── HTML → tree ──────────────────────────────────────────────────────────────
# A DOM small enough to walk by hand. Regexes got the website's own index far
# enough (one text blob per section); block structure needs real nesting — a
# table cell can hold a link holding bold text, and a callout holds paragraphs.
class Node:
    __slots__ = ("tag", "attrs", "kids", "text")

    def __init__(self, tag: str, attrs: dict | None = None, text: str = ""):
        self.tag = tag                       # "" for a text node
        self.attrs = attrs or {}
        self.kids: list[Node] = []
        self.text = text

    def cls(self) -> str:
        return self.attrs.get("class", "")

    def has_class(self, name: str) -> bool:
        return name in self.cls().split()

    def __repr__(self) -> str:  # debugging only
        return f"<{self.tag or 'text'} {self.cls()!r} kids={len(self.kids)}>"


# Void elements never get a closing tag; without this the parser would nest
# everything after an <img> inside it.
VOID = {"img", "br", "hr", "input", "meta", "link", "source", "col"}


class _TreeBuilder(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = Node("#root")
        self.stack = [self.root]

    def handle_starttag(self, tag, attrs):
        node = Node(tag, {k: (v or "") for k, v in attrs})
        self.stack[-1].kids.append(node)
        if tag not in VOID:
            self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        self.stack[-1].kids.append(Node(tag, {k: (v or "") for k, v in attrs}))

    def handle_endtag(self, tag):
        # Tolerate the stray unmatched close: pop to the nearest matching open,
        # or ignore it. Hand-written HTML has one of these eventually, and a
        # crash here would be a build break for a cosmetic typo.
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i].tag == tag:
                del self.stack[i:]
                return

    def handle_data(self, data):
        if data:
            self.stack[-1].kids.append(Node("", text=data))


def parse_fragment(html: str) -> Node:
    b = _TreeBuilder()
    b.feed(html)
    b.close()
    return b.root


def find_first(node: Node, tag: str, cls: str | None = None) -> Node | None:
    for k in node.kids:
        if k.tag == tag and (cls is None or k.has_class(cls)):
            return k
        found = find_first(k, tag, cls)
        if found is not None:
            return found
    return None


# ── Inline runs ──────────────────────────────────────────────────────────────
# Prose in these pages is not plain: API names are <code>, the thing being named
# is <strong>, and cross-references are links. Dropping that formatting makes the
# reference tables — which are mostly code spans — a grey wall, so each block
# carries a list of styled runs instead of a string.
#
#   s: "" body · "b" bold · "i" italic · "c" code · "l" link (with "h" = href)
#
# Nested markup collapses to the OUTERMOST meaningful style (a link wrapping a
# <strong> stays one link run): ImGui has one font per run, so there is nothing
# a combination could render anyway.
INLINE_STYLE = {"strong": "b", "b": "b", "em": "i", "i": "i", "code": "c"}

WS = re.compile(r"\s+")

# ── Characters the editor's font cannot draw ─────────────────────────────────
# The website is read in a browser, which will find SOME font for an arrow or a
# check mark. The editor has exactly one face — Roboto Condensed Bold, deployed
# next to the executable — and a codepoint missing from it comes out as an empty
# box. Its cmap has no arrows, no check marks and none of the Mac key symbols,
# and those are common in the docs: 148 arrows and 42 check marks in the current
# set alone.
#
# So they are substituted HERE, once, for something the font does have, rather
# than left to be discovered one screenshot at a time. The typography the font
# does carry — em dash, ellipsis, middle dot, ×, ², curly quotes — is untouched:
# replacing that would make the offline manual read worse than the website for
# no reason.
FONT_SUBSTITUTIONS = {
    "→": "->",     # →  no arrows in the face at all
    "←": "<-",     # ←
    "↔": "<->",    # ↔
    "▸": "»", # ▸  menu paths: "View » Console"
    "▶": "»", # ▶
    "✅": "•", # ✅ feature tables: a bullet, since a tick is missing too
    "✓": "•", # ✓
    "✔": "•", # ✔
    "⌘": "Cmd",    # ⌘  the shortcut tables spell the Mac keys out instead
    "⇧": "Shift",  # ⇧
    "⌃": "Ctrl",   # ⌃
    "⌥": "Alt",    # ⌥
}


def substitute(text: str) -> str:
    for src, dst in FONT_SUBSTITUTIONS.items():
        if src in text:
            text = text.replace(src, dst)
    return text


def _runs(node: Node, style: str, href: str, out: list[dict]) -> None:
    for k in node.kids:
        if k.tag == "":
            text = substitute(WS.sub(" ", k.text))
            if not text:
                continue
            if out and out[-1]["s"] == style and out[-1].get("h", "") == href:
                out[-1]["t"] += text
            else:
                run = {"t": text, "s": style}
                if href:
                    run["h"] = href
                out.append(run)
        elif k.tag == "br":
            if out:
                out[-1]["t"] = out[-1]["t"].rstrip() + "\n"
        elif k.tag == "a":
            _runs(k, style or "l", href or k.attrs.get("href", ""), out)
        elif k.tag in INLINE_STYLE:
            _runs(k, style if style in ("l",) else INLINE_STYLE[k.tag], href, out)
        elif k.tag in ("script", "style", "svg"):
            continue
        else:
            _runs(k, style, href, out)


def runs(node: Node) -> list[dict]:
    """Inline runs of `node`, trimmed at both ends."""
    out: list[dict] = []
    _runs(node, "", "", out)
    while out and not out[0]["t"].strip():
        out.pop(0)
    while out and not out[-1]["t"].strip():
        out.pop()
    if out:
        out[0]["t"] = out[0]["t"].lstrip()
        out[-1]["t"] = out[-1]["t"].rstrip()
    return [r for r in out if r["t"]]


def runs_text(rs: list[dict]) -> str:
    return "".join(r["t"] for r in rs)


def plain(node: Node) -> str:
    return runs_text(runs(node))


def raw_text(node: Node) -> str:
    """Text with whitespace kept — for <pre>, where it is the content."""
    if node.tag == "":
        return node.text
    if node.tag in ("script", "style"):
        return ""
    return "".join(raw_text(k) for k in node.kids)


# ── Blocks ───────────────────────────────────────────────────────────────────
# What DocsPanel knows how to draw. Anything this function does not recognise is
# recursed into rather than dropped, so a new wrapper <div> on the website
# degrades to its contents instead of silently deleting a paragraph.
def blocks_of(node: Node, images: set[str]) -> list[dict]:
    out: list[dict] = []
    for k in node.kids:
        out.extend(block_of(k, images))
    return out


def block_of(k: Node, images: set[str]) -> list[dict]:
    tag, cls = k.tag, k.cls()

    if tag == "":
        # Loose text between elements: real in hand-written HTML, but always
        # whitespace here. Anything else would be a paragraph without a <p>.
        return [{"k": "p", "r": [{"t": WS.sub(" ", k.text).strip(), "s": ""}]}] \
            if k.text.strip() else []

    if tag == "p":
        if k.has_class("docs-eyebrow"):
            return []                      # already the section's eyebrow
        rs = runs(k)
        if not rs:
            return []
        return [{"k": "lead" if k.has_class("docs-lead") else "p", "r": rs}]

    if tag in ("h1", "h2"):
        # The section's own heading, already lifted out as its title — leaving it
        # in the body would print every heading twice in the reader.
        return []

    if tag in ("h3", "h4", "h5", "h6"):
        rs = runs(k)
        return [{"k": "h3", "r": rs}] if rs else []

    if tag in ("ul", "ol"):
        items = [runs(li) for li in k.kids if li.tag == "li"]
        items = [i for i in items if i]
        return [{"k": tag, "items": items}] if items else []

    if tag == "table":
        return [table_block(k)]

    if tag == "pre":
        return [{"k": "code", "text": dedent_code(raw_text(k))}]

    if tag == "img":
        src = k.attrs.get("src", "")
        if not src or src.startswith(("http:", "https:", "data:")):
            return []
        images.add(src)
        return [{"k": "figure", "src": figure_name(src), "alt": k.attrs.get("alt", "")}]

    if tag == "span" and (k.has_class("callout-icon") or k.has_class("pipeline-arrow")):
        return []

    if tag == "div":
        if k.has_class("docs-divider"):
            return []
        if k.has_class("callout"):
            tone = next((t for t in ("warning", "tip", "note") if k.has_class(t)), "note")
            inner = blocks_of(k, images)
            return [{"k": "callout", "tone": tone, "blocks": inner}] if inner else []
        if k.has_class("docs-code"):
            bar = find_first(k, "div", "docs-code-bar")
            title = ""
            if bar is not None:
                t = find_first(bar, "span", "docs-code-title")
                title = plain(t) if t is not None else ""
            pre = find_first(k, "pre")
            if pre is None:
                return []
            b = {"k": "code", "text": dedent_code(raw_text(pre))}
            if title:
                b["title"] = title
            return b["text"].strip() and [b] or []
        if k.has_class("pipeline-diagram"):
            return [flow_block(k)]
        if k.has_class("pipeline-node") or k.has_class("pipeline-target"):
            return []                      # consumed by flow_block
        if k.has_class("docs-pager"):
            return []                      # site navigation, not content
        return blocks_of(k, images)

    if tag == "a" and k.has_class("docs-tile"):
        h3 = find_first(k, "h3")
        p = find_first(k, "p")
        return [{
            "k": "tile",
            "title": plain(h3) if h3 is not None else plain(k),
            "sub": plain(p) if p is not None else "",
            "href": k.attrs.get("href", ""),
        }]

    if tag in ("script", "style", "svg", "noscript", "figcaption"):
        return []

    # Unknown wrapper (nav grids, <figure>, future markup): keep the contents.
    return blocks_of(k, images)


def table_block(node: Node) -> dict:
    head: list[list[dict]] = []
    rows: list[list[list[dict]]] = []
    for tr in iter_tag(node, "tr"):
        cells = [c for c in tr.kids if c.tag in ("td", "th")]
        if not cells:
            continue
        if all(c.tag == "th" for c in cells) and not head:
            head = [runs(c) for c in cells]
        else:
            rows.append([runs(c) for c in cells])
    return {"k": "table", "head": head, "rows": rows}


def iter_tag(node: Node, tag: str):
    for k in node.kids:
        if k.tag == tag:
            yield k
        else:
            yield from iter_tag(k, tag)


def flow_block(node: Node) -> dict:
    """The website's arrow diagrams, as an ordered list of labelled steps.

    ImGui has no place to draw a horizontal flow chart inside a docked panel, and
    the arrows carry no information the order does not — so the reader shows them
    as a numbered sequence and keeps every word.
    """
    steps = []
    for n in iter_tag(node, "div"):
        if n.has_class("pipeline-node"):
            label = find_first(n, "span", "pipeline-node-label")
            sub = find_first(n, "span", "pipeline-node-sub")
            steps.append({"label": plain(label) if label is not None else "",
                          "sub": plain(sub) if sub is not None else ""})
        elif n.has_class("pipeline-target"):
            name = find_first(n, "span", "pipeline-target-name")
            api = find_first(n, "span", "pipeline-target-api")
            steps.append({"label": plain(name) if name is not None else "",
                          "sub": plain(api) if api is not None else ""})
    return {"k": "flow", "steps": [s for s in steps if s["label"]]}


# ── Figures ──────────────────────────────────────────────────────────────────
# The website ships its screenshots at full retina width (2500 px, 4 MB for the
# eight of them). That is the wrong trade for something copied next to every
# editor build and shown in a docked panel a few hundred pixels wide, so each
# one is downscaled and re-encoded here. JPEG, not PNG: these are photographic
# renders and UI screenshots with no transparency, and the sky captures alone
# are 3 MB as PNG.
FIGURE_MAX_WIDTH = 1280
FIGURE_QUALITY = 88


def figure_name(src: str) -> str:
    return Path(src).stem + ".jpg"


# ── Figures the EDITOR draws for itself ──────────────────────────────────────
# The website illustrates its pages with screenshots of a real editor. Half of
# what a reader needs is more basic than that — WHERE a panel is — and a
# screenshot answers it badly: it also shows somebody's project, their panel
# widths and whatever scene they had open, and it goes stale the first time any
# of that changes.
#
# So the manual gets a second kind of figure: a labelled map of the layout with
# the panel in question picked out, drawn by the editor's own UI code and
# published by scripts/he_uishot.py --docs. This table places them; the images
# themselves live in EditorDeps/Docs/img like the website's, because the reader
# loads figures from one folder and does not care which kind they are.
#
# Kept short on purpose. A figure per section would be a picture-book, and the
# same map five times over teaches nothing after the first.
FIGURES = {
    "editor#layout": ("doc-layout.jpg",
                      "The editor's default layout: the panels and where they sit"),
    "editor#outliner": ("doc-layout-outliner.jpg",
                        "Where the World Outliner sits in the default layout"),
    "editor#details": ("doc-layout-details.jpg",
                       "Where the Details panel sits in the default layout"),
    "editor#content-browser": ("doc-layout-content.jpg",
                               "Where the Content Browser sits in the default layout"),
    "editor#viewport": ("doc-layout-scene.jpg",
                        "Where the Scene viewport sits in the default layout"),
}


def copy_figures(docs_dir: Path, img_dir: Path, images: set[str]) -> int:
    try:
        from PIL import Image
    except ImportError:
        # Ship them untouched rather than not at all — the reader only needs the
        # file to exist, and a maintainer without Pillow still gets a working
        # bundle (just a fatter one). The name still ends in .jpg; stb_image
        # sniffs the content, not the extension.
        print("  warning: Pillow not installed — copying figures at full size")
        n = 0
        for src in sorted(images):
            s = docs_dir / src
            if s.exists():
                shutil.copy2(s, img_dir / figure_name(src))
                n += 1
        return n

    n = 0
    for src in sorted(images):
        s = docs_dir / src
        if not s.exists():
            print(f"  warning: figure missing on the website: {src}")
            continue
        with Image.open(s) as im:
            im = im.convert("RGB")
            if im.width > FIGURE_MAX_WIDTH:
                h = round(im.height * FIGURE_MAX_WIDTH / im.width)
                im = im.resize((FIGURE_MAX_WIDTH, h), Image.LANCZOS)
            im.save(img_dir / figure_name(src), "JPEG",
                    quality=FIGURE_QUALITY, optimize=True, progressive=False)
        n += 1
    return n


def dedent_code(text: str) -> str:
    """Strip the HTML indentation a <pre> inherits from its place in the page."""
    lines = substitute(text.replace("\r\n", "\n")).split("\n")
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    indent = min((len(l) - len(l.lstrip()) for l in lines if l.strip()), default=0)
    return "\n".join(l[indent:] if l.strip() else "" for l in lines)


# ── Search text ──────────────────────────────────────────────────────────────
# One flat string per section, built once here so the editor does not walk the
# block tree on every keystroke. Table cells are joined with spaces, so a query
# still matches a row's term and its explanation together.
def block_text(b: dict) -> str:
    k = b["k"]
    if k in ("p", "lead", "h3"):
        return runs_text(b["r"])
    if k in ("ul", "ol"):
        return " ".join(runs_text(i) for i in b["items"])
    if k == "table":
        parts = [runs_text(c) for c in b["head"]]
        for row in b["rows"]:
            parts.extend(runs_text(c) for c in row)
        return " ".join(parts)
    if k == "code":
        return (b.get("title", "") + " " + b["text"]).strip()
    if k == "callout":
        return " ".join(block_text(x) for x in b["blocks"])
    if k == "flow":
        return " ".join(f"{s['label']} {s['sub']}" for s in b["steps"])
    if k == "figure":
        return b.get("alt", "")
    if k == "tile":
        return f"{b['title']} {b['sub']}"
    return ""


# ── Pages ────────────────────────────────────────────────────────────────────
MAIN_RE = re.compile(r'<main class="docs-content">(.*?)</main>', re.S)
SIDEBAR_RE = re.compile(r'<aside class="docs-sidebar">(.*?)</aside>', re.S)
TITLE_RE = re.compile(r"<title>(.*?)</title>", re.S)


def page_id(filename: str) -> str:
    return filename[:-5] if filename.endswith(".html") else filename


def convert_page(path: Path, images: set[str]) -> dict | None:
    html = path.read_text(encoding="utf-8")
    m = MAIN_RE.search(html)
    if not m:
        return None

    title = ""
    tm = TITLE_RE.search(html)
    if tm:
        title = TITLE_TAIL.sub("", parse_text(tm.group(1))).strip()

    root = parse_fragment(m.group(1))
    summary = ""
    sections = []
    for sec in iter_tag(root, "section"):
        sid = sec.attrs.get("id", "")
        if sid == "hero":
            # The page's own headline: its title and one-line summary, not a
            # searchable section of its own.
            h1 = find_first(sec, "h1")
            sub = find_first(sec, "p", "he-hero-sub")
            if h1 is not None:
                title = plain(h1) or title
            if sub is not None:
                summary = plain(sub)
            continue
        h2 = find_first(sec, "h2")
        eyebrow = find_first(sec, "p", "docs-eyebrow")
        blocks = blocks_of(sec, images)
        if not blocks:
            continue

        # The editor's own figure for this section, if there is one. Placed
        # after the opening paragraph rather than at the end: "where is it" is
        # the question the reader arrives with, not the one left over.
        fig = FIGURES.get(f"{page_id(path.name)}#{sid}")
        if fig:
            at = 1 if blocks and blocks[0]["k"] in ("lead", "p") else 0
            blocks.insert(at, {"k": "figure", "src": fig[0], "alt": fig[1]})

        sections.append({
            "id": sid,
            "title": plain(h2) if h2 is not None else sid.replace("-", " ").title(),
            "eyebrow": plain(eyebrow) if eyebrow is not None else "",
            "blocks": blocks,
            "text": " ".join(t for t in (block_text(b) for b in blocks) if t),
        })

    return {
        "id": page_id(path.name),
        "file": path.name,
        "title": title,
        "summary": summary,
        "sections": sections,
    }


def parse_text(html_fragment: str) -> str:
    """Plain text of a small HTML fragment (used for <title>)."""
    return plain(parse_fragment(html_fragment))


def read_groups(docs_dir: Path) -> list[dict]:
    """The sidebar's page grouping — the docs' own table of contents.

    Taken from a page rather than hand-listed here so the editor's manual is
    ordered and grouped exactly like the website, including a page added later.
    The "On this page" group is per-page and skipped; every remaining group is a
    real one ("Manual", "Reference").
    """
    for candidate in ("editor.html", "getting-started.html"):
        path = docs_dir / candidate
        if not path.exists():
            continue
        m = SIDEBAR_RE.search(path.read_text(encoding="utf-8"))
        if not m:
            continue
        root = parse_fragment(m.group(1))
        groups = []
        for g in iter_tag(root, "div"):
            if not g.has_class("docs-sidebar-group"):
                continue
            t = find_first(g, "p", "docs-sidebar-title")
            gtitle = plain(t) if t is not None else ""
            pages = []
            for a in iter_tag(g, "a"):
                href = a.attrs.get("href", "")
                if not href or href.startswith("#") or re.match(r"^[a-z]+:|^//", href):
                    continue
                pid = page_id(href.split("#")[0].split("/")[-1])
                if pid and pid not in pages:
                    pages.append(pid)
            if gtitle and pages:
                groups.append({"title": gtitle, "pages": pages})
        if groups:
            return groups
    return []


def build(docs_dir: Path) -> tuple[dict, set[str]]:
    images: set[str] = set()
    pages = []
    for path in sorted(docs_dir.glob("*.html")):
        if path.name in SKIP_PAGES:
            continue
        page = convert_page(path, images)
        if page is None:
            print(f"  skipped {path.name} (no <main class=\"docs-content\">)")
            continue
        if not page["sections"]:
            print(f"  skipped {path.name} (no sections)")
            continue
        pages.append(page)

    groups = read_groups(docs_dir)
    known = {p["id"] for p in pages}
    for g in groups:
        g["pages"] = [p for p in g["pages"] if p in known]
    grouped = {p for g in groups for p in g["pages"]}
    rest = [p["id"] for p in pages if p["id"] not in grouped]
    if rest:
        groups.append({"title": "More", "pages": rest})

    bundle = {
        "version": SCHEMA_VERSION,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
        "baseUrl": BASE_URL,
        "groups": groups,
        "pages": pages,
    }
    return bundle, images


def dump(bundle: dict) -> str:
    # Sorted keys and a trailing newline so a regeneration produces a diff of the
    # docs that changed, not of the whole file.
    return json.dumps(bundle, ensure_ascii=False, indent=1, sort_keys=True) + "\n"


def comparable(text: str) -> str:
    """The bundle minus its timestamp — what --check actually compares."""
    obj = json.loads(text)
    obj.pop("generated", None)
    return json.dumps(obj, ensure_ascii=False, sort_keys=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--docs", type=Path, default=DEFAULT_DOCS,
                    help=f"HorizonEngineDocs directory (default: {DEFAULT_DOCS})")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT,
                    help=f"bundle to write (default: {DEFAULT_OUT})")
    ap.add_argument("--check", action="store_true",
                    help="verify the committed bundle is up to date; write nothing")
    args = ap.parse_args()

    if not args.docs.is_dir():
        print(f"error: docs directory not found: {args.docs}\n"
              f"       The website checkout is a sibling of this repo; pass --docs "
              f"if yours is elsewhere.", file=sys.stderr)
        return 2

    print(f"reading {args.docs}")
    bundle, images = build(args.docs)
    text = dump(bundle)

    sections = sum(len(p["sections"]) for p in bundle["pages"])
    print(f"  {len(bundle['pages'])} pages, {sections} sections, "
          f"{len(images)} images, {len(text) // 1024} KB")

    if args.check:
        if not args.out.exists():
            print(f"error: {args.out} does not exist — run without --check",
                  file=sys.stderr)
            return 1
        if comparable(args.out.read_text(encoding="utf-8")) != comparable(text):
            print(f"error: {args.out} is stale — re-run scripts/build_docs_bundle.py",
                  file=sys.stderr)
            return 1
        print("bundle is up to date")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    print(f"wrote {args.out}")

    img_dir = args.out.parent / "img"
    img_dir.mkdir(parents=True, exist_ok=True)
    copied = copy_figures(args.docs, img_dir, images)
    total_kb = sum(f.stat().st_size for f in img_dir.glob("*")) // 1024
    print(f"wrote {copied} figure(s) to {img_dir} ({total_kb} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
