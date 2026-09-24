#!/usr/bin/env python3
"""Build the website's Engine API reference from the registry dump.

    scripts/script_api_docs/gen_reference.py [--registry R] [--docs D] [--check]

Writes, into the website checkout's HorizonEngineDocs/:

  scripting-reference.html   one page: calling conventions, project types, the
                             flat horizon.* functions, every script callback and
                             one section per registry group with every row —
                             its Lua/Python, HorizonCode and C++ name, pins,
                             action/query, permission, and the editor's own
                             description (HcNodeDocs, carried in registry.json)
  horizoncode-nodes.html     only the block between the GEN markers in
                             #engine-call: the group catalogue, counted from
                             the registry instead of by hand
  scripting-api.html         only the block between the GEN markers in #api:
                             the table of all flat functions, from flat.json
  scripting.html             only the block between the GEN markers in #api:
                             the list of horizon.<group>.* groups

Hand-written content lives next to this file in overlay/ and is merged, never
overwritten: sections/*.html (whole sections), flat.json, callbacks.json,
notes.json (per id: note, perm, script_sig, examples) and groups/<group>.html
(a group's intro). Regenerate after every registry change
(dump_engine_api.sh first), then run the website's build_docs_index.py and
scripts/build_docs_bundle.py.

Two facts are checked against the engine source on every run, so the page
cannot quietly drift from it: the flat function list (kHorizonFuncs in
ScriptContext.cpp) and the callback names (ScriptEngine.cpp for Lua,
PyScriptBackend.cpp for Python). --check exits non-zero if a written file
would change.
"""
from __future__ import annotations

import argparse
import html
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
OVERLAY = HERE / "overlay"
PAGE = "scripting-reference.html"


def find_docs_dir() -> Path:
    for base in [REPO, *REPO.parents]:
        cand = base.parent / "Website" / "HorizonEngineDocs"
        if cand.is_dir():
            return cand
    return REPO.parent / "Website" / "HorizonEngineDocs"


# Sidebar clusters, in page order. Every registry group must land in exactly
# one; a new group the list does not know goes to "Other" and is reported.
CLUSTERS = [
    ("Core and data", ["math", "string", "random", "time", "timer", "datetime",
                       "json", "debug", "log"]),
    ("World and storage", ["entity", "transform", "scene", "content", "save",
                           "fs", "prefs"]),
    ("Gameplay", ["physics", "nav", "movement", "locomotion", "player", "input"]),
    ("Presentation", ["animator", "particle", "audio", "camera", "material", "env"]),
    ("Interface and app", ["ui", "widget", "cursor", "theme", "window", "dialog",
                           "clipboard", "app", "process", "print", "db", "http"]),
    ("Multiplayer", ["net", "anticheat"]),
]

# Section title per group where the registry category would be ambiguous
# ("Debug" is both log and debug) or terse.
GROUP_TITLE = {"log": "Log", "fs": "File System", "env": "Environment",
               "net": "Multiplayer", "anticheat": "Anti-Cheat", "db": "Database",
               "nav": "Navigation", "ui": "UI Elements", "prefs": "Preferences",
               "particle": "Particles", "http": "HTTP", "json": "JSON",
               "datetime": "Date and Time"}

# Groups the text frontends do not expose as horizon.<group>.* (HE::api::
# isScriptGroup is false) and how a script reaches them instead.
NOT_SCRIPT_NOTE = {
    "transform": "Lua and Python have no <code>horizon.transform.*</code>: use the flat "
                 "<code>horizon.getPosition</code> / <code>setPosition</code> / "
                 "<code>getRotation</code> / <code>setRotation</code> / <code>getScale</code> / "
                 "<code>setScale</code>. The world-space rows are HorizonCode and C++ only.",
    "material": "Lua and Python have no <code>horizon.material.*</code>: use the flat "
                "<code>horizon.getMaterialParam</code> / <code>setMaterialParam</code>, which "
                "take the value as one to four numbers.",
    "cursor": "Lua and Python have no <code>horizon.cursor.*</code>: use the flat "
              "<code>horizon.showCursor()</code> / <code>horizon.hideCursor()</code>.",
    "log": "In Lua and Python this row is the flat function <code>horizon.log(message)</code>.",
}

TYPE_NAME = {"Float": "float", "Int": "int", "Bool": "bool", "String": "string",
             "Vec2": "vec2", "Vec3": "vec3", "Vec4": "vec4", "Color": "color",
             "Ref": "ref", "Enum": "enum", "Struct": "struct", "Transform": "transform"}


def esc(s: str) -> str:
    return html.escape(s, quote=False)


def slug(fid: str) -> str:
    """Anchor of a registry row: physics.addImpulse -> physics-addImpulse."""
    return fid.replace(".", "-") if "." in fid else f"fn-{fid}"


def pin(p: dict) -> str:
    return f"{p['name']}: {TYPE_NAME.get(p['type'], p['type'].lower())}{'[]' if p['array'] else ''}"


def has_array(f: dict) -> bool:
    return any(p["array"] for p in f["params"] + f["results"])


# ── Source cross-checks ───────────────────────────────────────────────────────
def src(rel: str) -> str:
    return (REPO / rel).read_text(encoding="utf-8")


def flat_from_source() -> list[str]:
    return re.findall(r'\{\s*"([a-zA-Z]+)",\s*lua_horizon_[a-zA-Z]+\s*\}',
                      src("src/HE_Scene/src/ScriptContext.cpp"))


def callbacks_from_source() -> tuple[set[str], set[str]]:
    # Every "onXxx" literal in ScriptEngine.cpp is a method name it looks up on
    # the instance table; every "on_xxx" in the Python backend likewise.
    lua = set(re.findall(r'"(on[A-Z][A-Za-z0-9]*)"', src("src/HE_Core/src/Scripting/ScriptEngine.cpp")))
    py = set(re.findall(r'"(on_[a-z_0-9]+)"', src("src/HE_Python/src/PyScriptBackend.cpp")))
    # onRep_<var> is built at runtime from a prefix (ScriptContext.cpp).
    if '"onRep_"' in src("src/HE_Scene/src/ScriptContext.cpp"):
        lua.add("onRep_")
    return lua, py


def cb_name(sig: str) -> str:
    """onRep_&lt;var&gt;(self, old) -> onRep_; a remote call's <name>(…) -> ""."""
    name = html.unescape(sig).split("(")[0].replace("<var>", "")
    return "" if name.startswith("<") else name


# ── Doc text: HorizonCode display names become links to their rows ────────────
def build_linker(reg: list[dict]):
    counts: dict[str, int] = {}
    for f in reg:
        counts[f["display"]] = counts.get(f["display"], 0) + 1
    # Multi-word names only: "Log" or "Move" would hit ordinary prose. Longest
    # first, so "Play Animation Looped" wins over "Play Animation".
    names = sorted((f["display"] for f in reg
                    if " " in f["display"] and counts[f["display"]] == 1), key=len, reverse=True)
    target = {f["display"]: f["id"] for f in reg}
    pat = re.compile(r"(?<![\w-])(" + "|".join(re.escape(esc(n)) for n in names) + r")(?![\w-])")
    # Only the HorizonCode events a Lua/Python script also receives link to the
    # callbacks; the rest (On Menu Item, On Http Response, …) are graph-only and
    # stay plain text — the conventions section says so.
    event = re.compile(r"(?<![\w-])(On (?:Timer|Cheat Detected|Rep))(?![\w-])")

    def link(text: str, own: str) -> str:
        out = esc(text)

        def sub(m: re.Match) -> str:
            name = html.unescape(m.group(1))
            fid = target[name]
            if fid == own:
                return m.group(1)
            return f'<a href="#{slug(fid)}">{m.group(1)}</a>'
        out = pat.sub(sub, out)
        # HorizonCode events ("On Timer") are callbacks in a script.
        out = event.sub(lambda m: f'<a href="#callbacks">{m.group(1)}</a>', out)
        return out
    return link


# ── Page pieces ───────────────────────────────────────────────────────────────
def head(title: str, desc: str) -> str:
    return f"""<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>{title} — Horizon Engine Documentation</title>
    <meta
      name="description"
      content="{desc}"
    />
    <meta property="og:title" content="{title} — Horizon Engine" />
    <meta
      property="og:description"
      content="{desc}"
    />
    <meta
      property="og:image"
      content="https://horizoncreations.dev/HorizonEngine/Engine_Day.png"
    />
    <meta
      property="og:url"
      content="https://horizonengine.horizoncreations.dev/{PAGE}"
    />
    <meta property="og:type" content="website" />
    <link rel="stylesheet" href="https://horizoncreations.dev/style.css" />
    <link rel="stylesheet" href="docs.css?v=8" />
    <noscript>
      <!-- Without JS the language bar cannot do anything, so it goes away and
           every panel is shown, one under the other. -->
      <style>
        .docs-langs-bar {{ display: none !important; }}
        .docs-langs > .docs-lang-panel {{ display: block !important; margin-bottom: 0.75rem; }}
      </style>
    </noscript>
  </head>
  <body>
    <nav>
      <a href="index.html">
        <img class="nav-logo" src="HE_Logo.png" alt="Horizon Engine" />
      </a>
      <ul class="nav-links">
        <li>
          <a href="index.html">Overview</a>
        </li>
        <li>
          <a href="getting-started.html">Getting Started</a>
        </li>
        <li>
          <a href="editor.html">Manual</a>
        </li>
        <li class="active">
          <a href="scripting-api.html">Reference</a>
        </li>
        <li>
          <a href="https://horizoncreations.dev/HorizonEngine/HE.html">Engine ↗</a>
        </li>
      </ul>
      <button class="nav-hamburger" aria-label="Toggle navigation">
        <span></span><span></span><span></span>
      </button>
    </nav>
"""


def foot() -> str:
    return """
        <div class="docs-pager" role="navigation" aria-label="Documentation pages">
          <a class="pager-prev" href="scripting-api.html">
            <span class="pager-label">← Previous</span>
            <span class="pager-title">Scripting API Reference</span>
          </a>
          <a class="pager-next" href="horizoncode-nodes.html">
            <span class="pager-label">Next →</span>
            <span class="pager-title">HorizonCode Node Reference</span>
          </a>
        </div>
      </main>
    </div>

    <footer>
      <div class="footer-inner">
        <span>© 2026 Horizon Creations</span>
        <div class="footer-links">
          <a href="https://horizoncreations.dev/Legal/Impressum.html">Impressum</a>
          <a href="https://horizoncreations.dev/Legal/Privacy.html">Privacy Policy</a>
        </div>
      </div>
    </footer>

    <script src="https://horizoncreations.dev/nav.js"></script>
    <script src="scroll.js"></script>
    <script src="docs-nav.js?v=8"></script>
  </body>
</html>
"""


def code_panel(uid: str, lang: str, label: str, fname: str, code: str, first: bool) -> tuple[str, str]:
    btn = (f'<button type="button" class="docs-lang-btn{" active" if first else ""}" role="tab"\n'
           f'                      data-lang="{lang}" id="tab-{uid}-{lang}"'
           f'{"" if first else " tabindex=\"-1\""}\n'
           f'                      aria-selected="{"true" if first else "false"}" '
           f'aria-controls="panel-{uid}-{lang}">{label}</button>')
    panel = f"""            <div class="docs-code docs-lang-panel" data-lang="{lang}" role="tabpanel"
                 id="panel-{uid}-{lang}" aria-labelledby="tab-{uid}-{lang}">
              <div class="docs-code-bar">
                <span class="dot red"></span><span class="dot yellow"></span
                ><span class="dot green"></span>
                <span class="docs-code-title">{esc(fname)}</span>
              </div>
              <pre><code>{esc(code.rstrip())}</code></pre>
            </div>"""
    return btn, panel


def examples_block(uid: str, ex: dict) -> str:
    langs = [("lua", "Lua", ".lua"), ("python", "Python", ".py"),
             ("cpp", "C++", ".cpp"), ("horizoncode", "HorizonCode", "")]
    present = [l for l in langs if l[0] in ex]
    if not present:
        return ""
    btns, panels = [], []
    for i, (lang, label, ext) in enumerate(present):
        b, p = code_panel(uid, lang, label, ex.get("file", uid) + ext, ex[lang], i == 0)
        btns.append("              " + b)
        panels.append(p)
    return (f'          <div class="docs-langs" data-lang="{present[0][0]}">\n'
            f'            <div class="docs-langs-bar" role="tablist" aria-label="Scripting language">\n'
            + "\n".join(btns) + "\n            </div>\n" + "\n".join(panels) + "\n          </div>\n")


def section_flat(flat: dict, reg_ids: set[str]) -> str:
    L = ["""        <section id="flat" class="reveal">
          <p class="docs-eyebrow">Lua and Python only</p>
          <h2>Flat horizon.* Functions</h2>
          <div class="docs-divider"></div>
          <p>
            Besides the groups, <code>horizon</code> carries 35 functions directly:
            <code>horizon.setPosition(...)</code> rather than
            <code>horizon.transform.setPosition(...)</code>. They predate the
            registry, exist only in Lua and Python, and keep a signature of their
            own: some arguments are <strong>optional</strong> (in brackets), and
            <code>raycast</code> returns a table instead of separate values. Where
            a registry row does the same job it is linked; for
            <code>transform</code>, <code>material</code>, <code>cursor</code> and
            <code>log</code> these functions are the only way in from a script.
          </p>"""]
    for g in flat["groups"]:
        L.append(f"          <h3>{esc(g['title'])}</h3>")
        if g.get("note"):
            L.append(f"          <p>{g['note']}</p>")
        L.append("""          <div class="docs-table-wrap">
            <table class="docs-table">
              <thead>
                <tr><th>Function</th><th>Returns</th><th>Description</th></tr>
              </thead>
              <tbody>""")
        for it in g["items"]:
            twin = it.get("twin")
            if twin and twin not in reg_ids:
                raise SystemExit(f"flat.json: twin '{twin}' of {it['name']} is not a registry id")
            same = f' Same job as <a href="#{slug(twin)}"><code>{esc(twin)}</code></a>.' if twin else ""
            L.append(f'                <tr id="flat-{it["name"]}"><td><code>horizon.{it["name"]}({esc(it["args"])})</code></td>'
                     f'<td>{esc(it["returns"])}</td><td>{it["desc"]}{same}</td></tr>')
        L.append("""              </tbody>
            </table>
          </div>""")
    L.append("        </section>\n")
    return "\n".join(L)


def section_callbacks(cbs: dict) -> str:
    L = ["""        <section id="callbacks" class="reveal">
          <p class="docs-eyebrow">What the engine calls on your script</p>
          <h2>Callbacks</h2>
          <div class="docs-divider"></div>
          <p>
            A Lua script returns a table of functions, a Python script defines one
            subclass of <code>horizon.Behavior</code>. The engine calls the
            functions below on it by name; every one is optional, and a missing one
            is simply skipped. Lua names are camelCase, Python names snake_case.
            <strong>Receives</strong> says who gets the call: <em>this entity</em>
            means only the script on the entity concerned, <em>every script</em>
            means every Lua and Python instance in the session, so check the
            arguments before reacting. Results of many calls on this page arrive
            here: timers, input actions, network events, anti-cheat reports.
          </p>"""]
    for g in cbs["groups"]:
        L.append(f"          <h3>{esc(g['title'])}</h3>")
        if g.get("note"):
            L.append(f"          <p>{g['note']}</p>")
        L.append("""          <div class="docs-table-wrap">
            <table class="docs-table">
              <thead>
                <tr><th>Lua / Python</th><th>Receives</th><th>When</th></tr>
              </thead>
              <tbody>""")
        for it in g["items"]:
            to = "this entity" if it["to"] == "entity" else "every script"
            L.append(f'                <tr><td><code>{it["lua"]}</code><br><code>{it["python"]}</code></td>'
                     f'<td>{to}</td><td>{it["when"]}</td></tr>')
        L.append("""              </tbody>
            </table>
          </div>""")
    L.append("        </section>\n")
    return "\n".join(L)


def cluster_of(groups: list[str]) -> list[tuple[str, list[str]]]:
    known = {g for _, gs in CLUSTERS for g in gs}
    out = [(name, [g for g in gs if g in groups]) for name, gs in CLUSTERS]
    other = sorted(g for g in groups if g not in known)
    if other:
        print(f"note: groups not in CLUSTERS, listed under Other: {other}", file=sys.stderr)
        out.append(("Other", other))
    return [(n, gs) for n, gs in out if gs]


def title_of(group: str, rows: list[dict]) -> str:
    return GROUP_TITLE.get(group, rows[0]["category"])


def section_index(by: dict[str, list[dict]], clusters) -> str:
    L = ["""        <section id="groups" class="reveal">
          <p class="docs-eyebrow">The whole registry at a glance</p>
          <h2>All Groups</h2>
          <div class="docs-divider"></div>
          <div class="docs-table-wrap">
            <table class="docs-table">
              <thead>
                <tr><th>Group</th><th>Functions</th><th>In Lua / Python</th></tr>
              </thead>
              <tbody>"""]
    for _, gs in clusters:
        for g in gs:
            rows = by[g]
            if "." not in rows[0]["id"]:
                where = f"<code>horizon.{g}</code> (flat)"
            elif rows[0]["script"]:
                where = f"<code>horizon.{g}.*</code>"
            else:
                where = "flat functions only"
            L.append(f'                <tr><td><a href="#{g}">{esc(title_of(g, rows))}</a></td>'
                     f"<td>{len(rows)}</td><td>{where}</td></tr>")
    L.append("""              </tbody>
            </table>
          </div>
        </section>
""")
    return "\n".join(L)


def row_html(f: dict, notes: dict, link) -> str:
    g = f["group"]
    fn = f["id"].split(".", 1)[1] if "." in f["id"] else f["id"]
    n = notes.get(f["id"], {})
    script = f["script"] and "." in f["id"]
    sig = n.get("script_sig") or f"{fn}({', '.join(pin(p) for p in f['params'])})"
    ret = n.get("script_returns") or (", ".join(pin(p) for p in f["results"]) or "—")
    desc = link(f["doc"], f["id"])
    if n.get("note"):
        desc += " " + n["note"]
    meta = []
    if script:
        meta.append(f"Lua/Python <code>horizon.{esc(f['id'])}</code>")
    elif "." not in f["id"]:
        meta.append(f"Lua/Python <code>horizon.{esc(f['id'])}</code> (flat)")
    meta.append(f"HorizonCode <em>{esc(f['display'])}</em>")
    meta.append(f"C++ <code>{esc(f['cpp'])}</code>")
    meta.append("action" if f["exec"] else "query")
    selfp = [p["name"] for p in f["params"] if p["self"]]
    if selfp:
        meta.append(f'<a href="#entity-ids">{esc(selfp[0])}</a> is Self in HorizonCode')
    if has_array(f) and (script or "." not in f["id"]):
        meta.append('<a href="#arrays"><strong>list: HorizonCode/C++ only</strong></a>')
    if n.get("perm"):
        meta.append(f'needs permission <a href="#permissions">{esc(n["perm"])}</a>')
    cells = (f'<td><code>{esc(sig)}</code></td><td>{esc(ret)}</td>'
             f'<td>{desc}<br>{" · ".join(meta)}</td>')
    out = f'                <tr id="{slug(f["id"])}">{cells}</tr>'
    return out


def section_group(g: str, rows: list[dict], cluster: str, notes: dict, link) -> str:
    title = title_of(g, rows)
    count = f"{len(rows)} function{'s' if len(rows) != 1 else ''}"
    L = [f'        <section id="{g}" class="reveal">',
         f'          <p class="docs-eyebrow">{esc(cluster)} · {count}</p>',
         f"          <h2>{esc(title)}</h2>",
         '          <div class="docs-divider"></div>']
    if "." in rows[0]["id"] and rows[0]["script"]:
        L.append(f"          <p>Lua / Python: <code>horizon.{g}.&lt;function&gt;</code> · "
                 f"C++: <code>HE::api::{g}::&lt;function&gt;(ctx, …)</code></p>")
    elif g in NOT_SCRIPT_NOTE:
        L.append(f'          <div class="callout note">\n            <span class="callout-icon">◆</span>\n'
                 f"            <p>{NOT_SCRIPT_NOTE[g]}</p>\n          </div>")
    intro = OVERLAY / "groups" / f"{g}.html"
    if intro.is_file():
        L.append(intro.read_text(encoding="utf-8").rstrip())
    for f in rows:
        ex = notes.get(f["id"], {}).get("examples")
        if ex:
            L.append(f'          <h3 id="ex-{slug(f["id"])}">Example: {esc(f["id"])}</h3>')
            L.append(examples_block(f"ex-{slug(f['id'])}", ex).rstrip())
    if g == "env":
        L.append(env_table(rows, link))
    else:
        L.append("""          <div class="docs-table-wrap">
            <table class="docs-table">
              <thead>
                <tr><th>Function</th><th>Returns</th><th>Description</th></tr>
              </thead>
              <tbody>""")
        L += [row_html(f, notes, link) for f in rows]
        L.append("""              </tbody>
            </table>
          </div>""")
    L.append("        </section>\n")
    return "\n".join(L)


def env_table(rows: list[dict], link) -> str:
    """env.* is get/set pairs over the sky and weather fields: one line per field."""
    fields: dict[str, dict] = {}
    for f in rows:
        m = re.match(r"env\.(get|set)(.+)", f["id"])
        fields.setdefault(m.group(2), {})[m.group(1)] = f
    L = [f"""          <p>
            Every sky and weather field of the scene's Environment component, as a
            <code>get</code>/<code>set</code> pair: {len(fields)} fields,
            {len(rows)} functions. <code>horizon.env.getFogDensity()</code> reads
            the field, <code>horizon.env.setFogDensity(value)</code> writes it;
            a <code>color</code> or <code>vec3</code> field is spread into its
            numbers like any other argument. Setters are actions, getters queries,
            and both act on the active scene's environment.
          </p>
          <div class="docs-table-wrap">
            <table class="docs-table">
              <thead>
                <tr><th>Field</th><th>Type</th><th>Description</th></tr>
              </thead>
              <tbody>"""]
    for name, pair in fields.items():
        get, set_ = pair.get("get"), pair.get("set")
        ref = get or set_
        t = (get["results"][0]["type"] if get and get["results"]
             else set_["params"][-1]["type"])
        ids = " / ".join(f'<code id="{slug(x["id"])}">{esc(x["id"].split(".", 1)[1])}</code>'
                         for x in (get, set_) if x)
        L.append(f'                <tr><td>{esc(name)}<br>{ids}</td>'
                 f'<td>{TYPE_NAME.get(t, t.lower())}</td><td>{link(ref["doc"], ref["id"])}</td></tr>')
    L.append("""              </tbody>
            </table>
          </div>""")
    return "\n".join(L)


def sidebar(clusters, by) -> str:
    L = ["""      <aside class="docs-sidebar">
        <div class="docs-sidebar-group">
          <p class="docs-sidebar-title">Start here</p>
          <a class="docs-sidebar-link active" href="#conventions">Calling Conventions</a>
          <a class="docs-sidebar-link" href="#user-types">Project Types</a>
          <a class="docs-sidebar-link" href="#flat">Flat Functions</a>
          <a class="docs-sidebar-link" href="#callbacks">Callbacks</a>
          <a class="docs-sidebar-link" href="#groups">All Groups</a>
        </div>"""]
    for name, gs in clusters:
        L.append(f'        <div class="docs-sidebar-group">\n          <p class="docs-sidebar-title">{esc(name)}</p>')
        for g in gs:
            L.append(f'          <a class="docs-sidebar-link" href="#{g}">{esc(title_of(g, by[g]))}</a>')
        L.append("        </div>")
    L.append("""        <div class="docs-sidebar-group">
          <p class="docs-sidebar-title">More</p>
          <a class="docs-sidebar-link" href="scripting-api.html">Scripting API Reference</a>
          <a class="docs-sidebar-link" href="scripting.html">Scripting Guide</a>
          <a class="docs-sidebar-link" href="horizoncode-nodes.html">HorizonCode Node Reference</a>
        </div>
      </aside>
""")
    return "\n".join(L)


def build_page(reg: list[dict]) -> str:
    notes = {k: v for k, v in json.loads((OVERLAY / "notes.json").read_text(encoding="utf-8")).items()
             if not k.startswith("_")}
    flat = json.loads((OVERLAY / "flat.json").read_text(encoding="utf-8"))
    cbs = json.loads((OVERLAY / "callbacks.json").read_text(encoding="utf-8"))
    ids = {f["id"] for f in reg}
    unknown = sorted(k for k in notes if k not in ids)
    if unknown:
        raise SystemExit(f"notes.json names ids the registry does not have: {unknown}")

    # The page must say what the engine does, so both hand lists are held to
    # the source here rather than trusted.
    listed = [it["name"] for g in flat["groups"] for it in g["items"]]
    actual = flat_from_source()
    if sorted(listed) != sorted(actual):
        raise SystemExit(f"flat.json disagrees with kHorizonFuncs: missing "
                         f"{sorted(set(actual) - set(listed))}, extra {sorted(set(listed) - set(actual))}")
    lua_src, py_src = callbacks_from_source()
    lua_doc = {cb_name(it["lua"]) for g in cbs["groups"] for it in g["items"]} - {""}
    py_doc = {cb_name(it["python"]) for g in cbs["groups"] for it in g["items"]} - {""}
    if lua_doc != lua_src or py_doc != py_src:
        raise SystemExit("callbacks.json disagrees with the source:\n"
                         f"  Lua missing {sorted(lua_src - lua_doc)}, extra {sorted(lua_doc - lua_src)}\n"
                         f"  Python missing {sorted(py_src - py_doc)}, extra {sorted(py_doc - py_src)}")

    by: dict[str, list[dict]] = {}
    for f in reg:
        by.setdefault(f["group"], []).append(f)
    clusters = cluster_of(list(by))
    link = build_linker(reg)
    script_groups = sum(1 for g, rs in by.items() if rs[0]["script"] and "." in rs[0]["id"])

    desc = (f"Every engine call Horizon Engine scripts can make: {len(reg)} functions in "
            f"{len(by)} groups with their Lua, Python, HorizonCode and C++ names, the "
            f"flat horizon.* functions and every script callback.")
    P = [head("Engine API Reference", desc)]
    P.append(f"""    <section id="hero" class="he-hero docs-hero">
      <div class="he-hero-content">
        <p class="he-hero-eyebrow">Reference</p>
        <h1>Engine API</h1>
        <p class="he-hero-sub">
          All {len(reg)} engine functions in {len(by)} groups, each with its Lua,
          Python, HorizonCode and C++ name, plus the {len(listed)} flat
          <code>horizon.*</code> functions and every callback a script receives.
        </p>
      </div>
    </section>

    <div class="docs-layout">
""")
    P.append(sidebar(clusters, by))
    P.append('      <main class="docs-content">\n')
    P.append("        <!-- Generated by scripts/script_api_docs/gen_reference.py in the engine\n"
             "             repository from its API registry dump. Do not edit by hand: change\n"
             "             the overlay/ files there and regenerate. -->\n")
    P.append((OVERLAY / "sections" / "conventions.html").read_text(encoding="utf-8").rstrip() + "\n")
    P.append(section_flat(flat, ids))
    P.append(section_callbacks(cbs))
    P.append(section_index(by, clusters))
    for name, gs in clusters:
        for g in gs:
            P.append(section_group(g, by[g], name, notes, link))
    P.append(foot())
    page = "\n".join(P)
    # Numbers the hand-written overlay states in prose must match the registry.
    page = page.replace("{SCRIPT_GROUPS}", str(script_groups))
    return page


# ── horizoncode-nodes.html: the engine-call catalogue ────────────────────────
GEN_BEGIN = "<!-- GEN:engine-call-catalogue (scripts/script_api_docs/gen_reference.py) -->"
GEN_END = "<!-- /GEN:engine-call-catalogue -->"


def catalogue_block(reg: list[dict]) -> str:
    by: dict[str, list[dict]] = {}
    for f in reg:
        by.setdefault(f["group"], []).append(f)
    clusters = cluster_of(list(by))
    L = [GEN_BEGIN,
         "          <p>",
         "            One descriptor registry lights up <code>Engine Call</code> nodes and the",
         "            <code>horizon.&lt;group&gt;.&lt;fn&gt;</code> Lua/Python APIs at the same",
         "            time, so a HorizonCode graph and a script reach exactly the same engine",
         f"            surface: <strong>{len(by)} groups, {len(reg)} functions</strong>. Every one",
         f'            is described, with its pins, in the <a href="{PAGE}">Engine API',
         "            Reference</a>; inside the editor, F1 on a node opens its entry.",
         "          </p>",
         '          <div class="docs-table-wrap">',
         '            <table class="docs-table">',
         "              <thead>",
         "                <tr><th>Group</th><th>#</th><th>Functions</th></tr>",
         "              </thead>",
         "              <tbody>"]
    for _, gs in clusters:
        for g in gs:
            rows = by[g]
            if g == "env":
                fns = (f"get/set for every sky and weather field, {len(rows) // 2} fields × get+set "
                       f'(<a href="{PAGE}#env">list</a>)')
            else:
                fns = ", ".join(f'<code>{esc(r["id"].split(".", 1)[-1])}</code>' for r in rows)
            L.append(f'                <tr><td><a href="{PAGE}#{g}">{esc(title_of(g, rows))}</a></td>'
                     f"<td>{len(rows)}</td><td>{fns}</td></tr>")
    L += ["              </tbody>", "            </table>", "          </div>", "          " + GEN_END]
    return "\n".join(L)


def patch_catalogue(text: str, reg: list[dict]) -> str:
    block = catalogue_block(reg)
    if GEN_BEGIN in text:
        a = text.index(GEN_BEGIN)
        b = text.index(GEN_END, a) + len(GEN_END)
        return text[:a] + block + text[b:]
    # First run: replace the hand-written intro paragraph + table.
    sec = text.index('id="engine-call"')
    a = text.index("<p>", text.index('<div class="docs-divider"></div>', sec))
    b = text.index("</table>", a)
    b = text.index("</div>", b) + len("</div>")
    return text[:a] + block + text[b:]


# ── scripting-api.html: the flat function table in #api ──────────────────────
FLAT_BEGIN = "<!-- GEN:flat-functions (scripts/script_api_docs/gen_reference.py) -->"
FLAT_END = "<!-- /GEN:flat-functions -->"


def flat_table_block() -> str:
    """All 35 flat functions as one table, from the same flat.json the reference
    page uses — the page that says "every horizon.* function" can then no longer
    list 13 of them."""
    flat = json.loads((OVERLAY / "flat.json").read_text(encoding="utf-8"))
    L = [FLAT_BEGIN,
         '          <div class="docs-table-wrap">',
         '            <table class="docs-table">',
         "              <thead>",
         "                <tr><th>Function</th><th>Returns</th><th>Description</th></tr>",
         "              </thead>",
         "              <tbody>"]
    for g in flat["groups"]:
        for it in g["items"]:
            L.append(f'                <tr><td><code>{it["name"]}({esc(it["args"])})</code></td>'
                     f'<td>{esc(it["returns"])}</td><td>{it["desc"]} '
                     f'<a href="{PAGE}#flat-{it["name"]}">Reference</a></td></tr>')
    L += ["              </tbody>", "            </table>", "          </div>", "          " + FLAT_END]
    return "\n".join(L)


def patch_flat_table(text: str) -> str:
    block = flat_table_block()
    if FLAT_BEGIN in text:
        a = text.index(FLAT_BEGIN)
        b = text.index(FLAT_END, a) + len(FLAT_END)
        return text[:a] + block + text[b:]
    # First run: the first table of the #api section.
    sec = text.index('<section id="api"')
    a = text.index('<div class="docs-table-wrap">', sec)
    b = text.index("</table>", a)
    b = text.index("</div>", b) + len("</div>")
    return text[:a] + block + text[b:]


# ── scripting.html: the namespaced groups paragraph in #api ──────────────────
GROUPS_BEGIN = "<!-- GEN:script-groups (scripts/script_api_docs/gen_reference.py) -->"
GROUPS_END = "<!-- /GEN:script-groups -->"


def script_groups_block(reg: list[dict]) -> str:
    by: dict[str, list[dict]] = {}
    for f in reg:
        by.setdefault(f["group"], []).append(f)
    groups = [g for _, gs in cluster_of(list(by)) for g in gs
              if by[g][0]["script"] and "." in by[g][0]["id"]]
    n = sum(len(by[g]) for g in groups)
    names = ", ".join(f'<a href="{PAGE}#{g}"><code>{g}</code></a>' for g in groups)
    return (f"{GROUPS_BEGIN}\n          <p>\n"
            f"            {len(groups)} groups with {n} functions, each as "
            f"<code>horizon.&lt;group&gt;.&lt;function&gt;</code>: {names}.\n"
            f"            <code>transform</code>, <code>material</code>, <code>cursor</code> and\n"
            f"            <code>log</code> are reached through the flat functions above.\n"
            f"          </p>\n          {GROUPS_END}")


def patch_script_groups(text: str, reg: list[dict]) -> str:
    block = script_groups_block(reg)
    if GROUPS_BEGIN in text:
        a = text.index(GROUPS_BEGIN)
        b = text.index(GROUPS_END, a) + len(GROUPS_END)
        return text[:a] + block + text[b:]
    # First run: the paragraph under the "Namespaced groups" heading.
    a = text.index("<p>", text.index("<h3>Namespaced groups</h3>"))
    b = text.index("</p>", a) + len("</p>")
    return text[:a] + block + text[b:]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--registry", type=Path, default=HERE / "registry.json")
    ap.add_argument("--docs", type=Path, default=find_docs_dir())
    ap.add_argument("--check", action="store_true", help="exit 1 if a file would change")
    a = ap.parse_args()

    reg = json.loads(a.registry.read_text(encoding="utf-8"))
    outputs = {a.docs / PAGE: build_page(reg)}
    nodes = a.docs / "horizoncode-nodes.html"
    outputs[nodes] = patch_catalogue(nodes.read_text(encoding="utf-8"), reg)
    api = a.docs / "scripting-api.html"
    outputs[api] = patch_flat_table(api.read_text(encoding="utf-8"))
    guide = a.docs / "scripting.html"
    outputs[guide] = patch_script_groups(guide.read_text(encoding="utf-8"), reg)

    stale = []
    for path, text in outputs.items():
        old = path.read_text(encoding="utf-8") if path.exists() else None
        if old == text:
            continue
        stale.append(path.name)
        if not a.check:
            path.write_text(text, encoding="utf-8")
    if a.check:
        print("up to date" if not stale else f"stale: {', '.join(stale)}")
        return 1 if stale else 0
    print(f"{len(reg)} ids -> {a.docs / PAGE}" + (f"; wrote {', '.join(stale)}" if stale else "; unchanged"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
