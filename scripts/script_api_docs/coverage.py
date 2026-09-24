#!/usr/bin/env python3
"""How much of the scripting surface the website docs actually cover.

Reads the registry dump (dump_engine_api.sh) and the website's HorizonEngineDocs
HTML and writes a markdown report: per registry group, which ids have a real
reference entry, which are only named somewhere, which are missing entirely —
plus the two surfaces the registry does not describe (the 35 flat horizon.*
shims and the lifecycle callbacks).

    scripts/script_api_docs/coverage.py [--registry R] [--docs D] [--out O]

Status per id, strongest first:
  ref      a signature + return value is documented (today only the flat twins
           in scripting-api.html#api, e.g. transform.setPosition -> setPosition)
  named    the literal id ("physics.addImpulse") appears in a docs page
  catalog  the function name appears in the HorizonCode node catalogue row of its
           group (horizoncode-nodes.html#engine-call) — a name, nothing more
  missing  none of the above
"""
from __future__ import annotations

import argparse
import collections
import html
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def find_docs_dir() -> Path:
    for base in [REPO, *REPO.parents]:
        cand = base.parent / "Website" / "HorizonEngineDocs"
        if cand.is_dir():
            return cand
    return REPO.parent / "Website" / "HorizonEngineDocs"


# Registry id -> flat horizon.* twin, for rows whose twin has a documented signature.
FLAT_TWIN = {
    "log": "log",
    "entity.getName": "getName", "entity.spawn": "spawn", "entity.destroy": "destroy",
    "transform.getPosition": "getPosition", "transform.setPosition": "setPosition",
    "transform.getRotation": "getRotation", "transform.setRotation": "setRotation",
    "transform.getScale": "getScale", "transform.setScale": "setScale",
    "physics.raycast": "raycast", "physics.setVelocity": "setVelocity",
    "physics.isGrounded": "isGrounded",
    "material.getParam": "getMaterialParam", "material.setParam": "setMaterialParam",
    "ui.getText": "getUIText", "ui.setText": "setUIText",
    "ui.getColor": "getUIColor", "ui.setColor": "setUIColor",
    "ui.getVisible": "isUIVisible", "ui.setVisible": "setUIVisible",
    "ui.getPosition": "getUIPosition", "ui.setPosition": "setUIPosition",
    "ui.getSize": "getUISize", "ui.setSize": "setUISize",
    "ui.setMaterialParam": "setUIMaterialParam",
    "widget.setZOrder": "setWidgetZOrder", "widget.isVisible": "isWidgetVisible",
    "widget.callFunction": "callWidgetFunction",
    "cursor.setVisible": "showCursor/hideCursor",
}


def page_texts(docs: Path) -> dict[str, str]:
    out = {}
    for p in sorted(docs.glob("*.html")):
        raw = p.read_text(encoding="utf-8")
        m = re.search(r'<main class="docs-content">(.*)</main>', raw, re.S)
        body = m.group(1) if m else raw
        # Inline tags vanish without a space, so highlighted code
        # (horizon.<span>physics</span>.addImpulse) still reads as one token.
        body = re.sub(r"</?(span|code|em|strong|b|i|a)\b[^>]*>", "", body)
        out[p.name] = html.unescape(re.sub(r"<[^>]+>", " ", body))
    return out


def catalogue(docs: Path) -> dict[str, tuple[str, str]]:
    raw = (docs / "horizoncode-nodes.html").read_text(encoding="utf-8")
    at = raw.find('id="engine-call"')
    if at < 0:
        return {}
    cat = {}
    for tr in re.findall(r"<tr>(.*?)</tr>", raw[at:], re.S):
        tds = re.findall(r"<td[^>]*>(.*?)</td>", tr, re.S)
        if len(tds) >= 3:
            strip = lambda s: html.unescape(re.sub(r"<[^>]+>", " ", s)).strip()
            cat[strip(tds[0])] = (strip(tds[1]), strip(tds[2]))
    return cat


def flat_documented(docs: Path) -> set[str]:
    raw = (docs / "scripting-api.html").read_text(encoding="utf-8")
    at = raw.find('id="api"')
    end = raw.find("<section", at + 1)
    return set(re.findall(r"<code>([a-zA-Z]+)\(", raw[at:end if end > 0 else None]))


def in_catalogue(fn: str, row: str) -> bool:
    if re.search(r"\b" + re.escape(fn) + r"\b", row):
        return True
    base = re.sub(r"^(get|set|is)(?=[A-Z])", "", fn)
    return (base != fn and re.search(r"\b" + re.escape(base) + r"\b", row, re.I) is not None
            and re.search(r"get\s*/\s*set|get\+set", row, re.I) is not None)


def sig(f: dict) -> str:
    def pin(p):
        t = p["type"] + ("[]" if p["array"] else "")
        return f"{p['name']}{'=self' if p['self'] else ''}: {t}"
    res = ", ".join(pin(p) for p in f["results"])
    return f"`{f['id']}({', '.join(pin(p) for p in f['params'])})`" + (f" → {res}" if res else "")


def lifecycle_callbacks() -> tuple[list[str], list[str]]:
    lua = (REPO / "src/HE_Scene/src/ScriptContext.cpp").read_text(encoding="utf-8")
    py = (REPO / "src/HE_Python/src/PyScriptBackend.cpp").read_text(encoding="utf-8")
    lua_cb = sorted(set(re.findall(r'HE_SCRIPT_CALL\("(on[A-Za-z0-9_]*)"', lua)))
    py_cb = sorted(set(re.findall(r'"(on_[a-z_]+)"', py)))
    return lua_cb, py_cb


def flat_functions() -> list[str]:
    lua = (REPO / "src/HE_Scene/src/ScriptContext.cpp").read_text(encoding="utf-8")
    return re.findall(r'\{\s*"([a-zA-Z]+)",\s*lua_horizon_[a-zA-Z]+\s*\}', lua)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--registry", type=Path, default=HERE / "registry.json")
    ap.add_argument("--docs", type=Path, default=find_docs_dir())
    ap.add_argument("--out", type=Path, default=REPO / "docs" / "script-api-docs-coverage.md")
    a = ap.parse_args()

    reg = json.loads(a.registry.read_text(encoding="utf-8"))
    texts = page_texts(a.docs)
    cat = catalogue(a.docs)
    flat_doc = flat_documented(a.docs)

    rows = []
    for f in reg:
        fn = f["id"].split(".", 1)[1] if "." in f["id"] else f["id"]
        twin = FLAT_TWIN.get(f["id"])
        pages = [p for p, t in texts.items()
                 if re.search(r"(?<!\w)" + re.escape(f["id"]) + r"(?![\w])", t)]
        c = cat.get(f["category"])
        if twin and all(t in flat_doc for t in twin.split("/")):
            status = "ref"
        elif pages:
            status = "named"
        elif c and in_catalogue(fn, c[1]):
            status = "catalog"
        else:
            status = "missing"
        rows.append({**f, "fn": fn, "status": status, "pages": pages, "twin": twin})

    by = collections.defaultdict(list)
    for r in rows:
        by[r["group"]].append(r)
    order = sorted(by, key=lambda g: (-len(by[g]), g))

    L = []
    w = L.append
    w("# Script-API-Doku: Abdeckung je Registry-Id (generiert)")
    w("")
    w("Erzeugt von `scripts/script_api_docs/coverage.py` aus `scripts/script_api_docs/registry.json` "
      "(Dump von `HE::api::registry()`) und der lokalen Website-Doku. Nicht von Hand pflegen, neu erzeugen. "
      "Einordnung und Folgeschritte: `docs/script-api-docs-gap-audit-2026-09-24.md`.")
    w("")
    w("Status: **ref** = Signatur+Rückgabe dokumentiert (nur über den flachen Zwilling), **named** = Id "
      "wörtlich auf einer Doku-Seite, **catalog** = nur der Name im HorizonCode-Knotenkatalog, **missing** = nirgends.")
    w("")
    tot = collections.Counter(r["status"] for r in rows)
    w(f"**Gesamt: {len(rows)} Registry-Ids in {len(by)} Gruppen** — ref {tot['ref']}, named {tot['named']}, "
      f"catalog {tot['catalog']}, missing {tot['missing']}.")
    w("")
    w("| Gruppe | Kategorie | Ids | Lua/Py `horizon.<gruppe>.*` | ref | named | catalog | missing |")
    w("|---|---|---:|:---:|---:|---:|---:|---:|")
    for g in order:
        rs = by[g]
        cnt = collections.Counter(r["status"] for r in rs)
        w(f"| `{g}` | {rs[0]['category']} | {len(rs)} | {'ja' if rs[0]['script'] else '**nein**'} | "
          f"{cnt['ref']} | {cnt['named']} | {cnt['catalog']} | {cnt['missing']} |")
    w("")

    flat = flat_functions()
    w(f"## Flache `horizon.*`-Funktionen ({len(flat)}, nicht in der Registry)")
    w("")
    w("Hand geschriebene Shims, identisch in Lua (`ScriptContext.cpp`) und Python (`PyScriptBackend.cpp`).")
    w("")
    doc = [x for x in flat if x in flat_doc]
    und = [x for x in flat if x not in flat_doc]
    w(f"- dokumentiert mit Signatur ({len(doc)}): " + ", ".join(f"`{x}`" for x in doc))
    w(f"- **ohne Signatur** ({len(und)}): " + ", ".join(f"`{x}`" for x in und))
    w("")

    lua_cb, py_cb = lifecycle_callbacks()
    alltext = "\n".join(texts.values())
    w("## Lifecycle-Callbacks")
    w("")
    w("Aus dem Code gelesen (Lua: `HE_SCRIPT_CALL`-Namen, Python: `on_*`-Strings). Dokumentiert = Name "
      "kommt auf einer Doku-Seite vor.")
    w("")
    w("- Lua: " + ", ".join(f"`{c}`" + ("" if c in alltext else " **fehlt**") for c in lua_cb))
    w("- Python: " + ", ".join(f"`{c}`" + ("" if c in alltext else " **fehlt**") for c in py_cb))
    w("")

    w("## Je Gruppe")
    for g in order:
        rs = by[g]
        w("")
        w(f"### `{g}` — {rs[0]['category']} ({len(rs)})")
        if not rs[0]["script"]:
            w("")
            w(f"> Nicht in `isScriptGroup()`: in Lua/Python gibt es **kein** `horizon.{g}.*`; "
              "erreichbar nur über HorizonCode, C++ und ggf. einen flachen Zwilling.")
        w("")
        if g == "env":
            fields = collections.OrderedDict()
            for r in rs:
                m = re.match(r"(get|set)(.*)", r["fn"])
                key = m.group(2) if m else r["fn"]
                fields.setdefault(key, []).append(r)
            w(f"{len(fields)} Felder, je get+set. Kandidat für eine generierte Tabelle statt Einzeleinträgen.")
            w("")
            w("| Feld | Typ | Status |")
            w("|---|---|---|")
            for k, fr in fields.items():
                t = next((p["type"] for r in fr for p in r["results"] + r["params"][-1:]), "?")
                st = "/".join(sorted({r["status"] for r in fr}))
                w(f"| {k} | {t} | {st} |")
            continue
        w("| Id / Signatur | Art | Status | Beleg |")
        w("|---|---|---|---|")
        for r in rs:
            art = "exec" if r["exec"] else "pure"
            beleg = ", ".join(r["pages"]) if r["pages"] else ""
            if r["status"] == "ref":
                beleg = f"flach `{r['twin']}` (scripting-api.html#api)"
            elif r["status"] == "catalog":
                beleg = "horizoncode-nodes.html#engine-call"
            w(f"| {sig(r)} | {art} | {r['status']} | {beleg} |")

    a.out.write_text("\n".join(L) + "\n", encoding="utf-8")
    print(f"{len(rows)} ids, {dict(tot)} -> {a.out}")


if __name__ == "__main__":
    main()
