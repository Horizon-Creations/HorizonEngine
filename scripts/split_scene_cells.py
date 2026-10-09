#!/usr/bin/env python3
"""Split a .hescene into a base scene plus a grid of cell scenes (Thema 153).

The game (GameApplication + HE::CellStreamer) loads the base scene as usual and
then the cells by distance from the camera: a cell comes in once the camera, or
where it is heading, is within the load radius of the cell's square, and goes
again beyond the unload radius.

What moves into the cells: top-level subtrees whose every entity only carries
"placed thing" components (transform, mesh, material, point/spot light, LOD,
collider, STATIC rigid body, decal, inactive). Pure folders (no components, or
an identity transform only) are looked through: their children are placed one
by one, and a folder that ends up empty is dropped from the base. Everything
else stays in the base: sky, weather, terrain, cameras, scripts, characters,
dynamic bodies (they would be unloaded with the square they happened to start
in), prefab instances (the editor keeps those in sync), directional lights.

A subtree goes to the cell its top entity's position falls into. Positions are
absolute (a folder above it is identity), and stay as they are in the cell file.

    scripts/split_scene_cells.py <scene.hescene> --out <base.hescene>
        [--cell-size 512] [--load-radius 768] [--unload-radius 1024]
        [--lookahead 2] [--cells-dir <dir>] [--project-root <dir>]

--cells-dir defaults to <base stem>.cells next to --out; the manifest stores it
relative to the project root (the folder with the .heproj above it, or
--project-root), which is how the game finds the files, loose or packed.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

MOVABLE = {"transform", "mesh", "material", "light", "lod", "collider", "rigidbody", "decal", "inactive"}


def key(uuid) -> tuple:
    return tuple(uuid) if isinstance(uuid, list) else (uuid,)


def movable_entity(e: dict) -> bool:
    comps = e.get("components") or {}
    if not set(comps) <= MOVABLE:
        return False
    light = comps.get("light")
    if light is not None and light.get("type", 0) == 0:   # directional
        return False
    body = comps.get("rigidbody")
    if body is not None and body.get("type", 0) != 0:     # 0 = Static
        return False
    return True


def is_folder(e: dict) -> bool:
    comps = e.get("components") or {}
    if not e.get("children"):
        return False
    if not comps:
        return True
    if set(comps) != {"transform"}:
        return False
    t = comps["transform"]
    return (all(abs(v) < 1e-9 for v in t.get("position", [0, 0, 0]))
            and all(abs(v) < 1e-9 for v in t.get("rotation", [0, 0, 0]))
            and all(abs(v - 1.0) < 1e-9 for v in t.get("scale", [1, 1, 1])))


def find_project_root(start: Path) -> Path | None:
    for d in [start, *start.parents]:
        if any(d.glob("*.heproj")):
            return d
    return None


def split(scene: dict, cell_size: float):
    """Returns (base scene, {(x, z): [unit uuids]}, by_id). Mutates nothing."""
    entities = scene.get("entities", [])
    by_id = {key(e["uuid"]): e for e in entities}
    roots = [e for e in entities if not e.get("parent") or key(e["parent"]) not in by_id]
    if len(roots) != 1:
        raise SystemExit(f"expected one scene root, found {len(roots)}")

    def subtree_movable(e: dict) -> bool:
        if not movable_entity(e):
            return False
        return all(subtree_movable(by_id[key(c)]) for c in e.get("children", []) if key(c) in by_id)

    cells: dict[tuple, list] = {}
    moved: set = set()

    def visit(parent: dict):
        for c in parent.get("children", []):
            child = by_id.get(key(c))
            if child is None:
                continue
            if is_folder(child):
                visit(child)
            elif subtree_movable(child):
                pos = ((child.get("components") or {}).get("transform") or {}).get("position", [0, 0, 0])
                cell = (math.floor(pos[0] / cell_size), math.floor(pos[2] / cell_size))
                cells.setdefault(cell, []).append(key(child["uuid"]))
                moved.add(key(child["uuid"]))

    visit(roots[0])

    # Base: the scene without the moved subtrees and without folders left empty.
    def subtree_ids(k):
        e = by_id[k]
        yield k
        for c in e.get("children", []):
            if key(c) in by_id:
                yield from subtree_ids(key(c))

    gone = set()
    for k in moved:
        gone.update(subtree_ids(k))
    base_entities = []
    children_left = {}
    for e in entities:
        k = key(e["uuid"])
        if k in gone:
            continue
        kids = [c for c in e.get("children", []) if key(c) not in gone]
        children_left[k] = kids
    # Folders that had children and now have none, bottom-up.
    changed = True
    while changed:
        changed = False
        for e in entities:
            k = key(e["uuid"])
            if k in gone or e is roots[0]:
                continue
            if e.get("children") and not children_left[k] and is_folder(e):
                gone.add(k)
                changed = True
                pk = key(e["parent"]) if e.get("parent") else None
                if pk in children_left:
                    children_left[pk] = [c for c in children_left[pk] if key(c) != k]
    for e in entities:
        k = key(e["uuid"])
        if k in gone:
            continue
        copy = dict(e)
        if "children" in e:
            copy["children"] = children_left[k]
        base_entities.append(copy)
    base = dict(scene)
    base["entities"] = base_entities
    return base, cells, by_id


def cell_scene(scene: dict, cell: tuple, units: list, by_id: dict) -> dict:
    x, z = cell
    root_uuid = [0xCE11000000000000, ((x & 0xFFFFFFFF) << 32) | (z & 0xFFFFFFFF)]
    root = {"children": [list(u) for u in units], "components": {}, "name": f"Cell {x},{z}",
            "parent": None, "uuid": root_uuid}
    out = [root]

    def add(k, parent_uuid):
        e = dict(by_id[k])
        e["parent"] = parent_uuid
        out.append(e)
        for c in e.get("children", []):
            if key(c) in by_id:
                add(key(c), e["uuid"])

    for u in units:
        add(u, root_uuid)
    return {"entities": out, "version": scene.get("version", "1.1")}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("scene", type=Path)
    ap.add_argument("--out", type=Path, required=True, help="base scene to write")
    ap.add_argument("--cell-size", type=float, default=512.0)
    ap.add_argument("--load-radius", type=float, default=None, help="default 1.5 x cell size")
    ap.add_argument("--unload-radius", type=float, default=None, help="default 1.25 x load radius")
    ap.add_argument("--lookahead", type=float, default=2.0)
    ap.add_argument("--cells-dir", type=Path, default=None)
    ap.add_argument("--project-root", type=Path, default=None)
    a = ap.parse_args()

    scene = json.loads(a.scene.read_text())
    base, cells, by_id = split(scene, a.cell_size)
    cells_dir = a.cells_dir or a.out.with_name(a.out.stem + ".cells")
    project = a.project_root or find_project_root(a.out.resolve().parent)
    if project is None:
        raise SystemExit("no .heproj above --out; pass --project-root")
    try:
        rel_dir = cells_dir.resolve().relative_to(project.resolve()).as_posix()
    except ValueError:
        raise SystemExit(f"{cells_dir} is not inside the project {project}")

    load = a.load_radius if a.load_radius is not None else 1.5 * a.cell_size
    unload = a.unload_radius if a.unload_radius is not None else 1.25 * load
    cells_dir.mkdir(parents=True, exist_ok=True)
    listing = []
    moved_entities = 0
    for cell in sorted(cells):
        cs = cell_scene(scene, cell, cells[cell], by_id)
        n = len(cs["entities"]) - 1
        moved_entities += n
        (cells_dir / f"cell_{cell[0]}_{cell[1]}.hescene").write_text(json.dumps(cs, separators=(",", ":")))
        listing.append([cell[0], cell[1], n])
    base["cells"] = {"cellSize": a.cell_size, "loadRadius": load, "unloadRadius": unload,
                     "lookaheadSec": a.lookahead, "dir": rel_dir, "list": listing}
    a.out.write_text(json.dumps(base, indent=1))
    print(f"{a.scene}: {len(scene.get('entities', []))} entities -> base {len(base['entities'])}, "
          f"{len(cells)} cell(s) with {moved_entities} entities in {rel_dir} "
          f"(cell {a.cell_size:.0f} m, load {load:.0f} m, unload {unload:.0f} m)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
