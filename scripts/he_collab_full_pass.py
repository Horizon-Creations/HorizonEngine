#!/usr/bin/env python3
"""he_collab_full_pass — three real editors, every object type a session syncs.

he_collab_two_editors.py proves the session itself (host, join, snapshot, the
entity lock across processes). This run is the verification pass of topic 198
(docs/collab-sync-audit-verification-2026-10-14.md): three deployed
HorizonEditor processes, A hosting, B joined from the start, C joining late,
and every synced object type changed on one side and read back on another
through that editor's own MCP tools:

  1. entities: create, move, scale, a property (light intensity), rename,
     reparent, delete;
  2. a peer-held entity: delete and rename from the host are refused
     (locked_by_other), the holder still has it;
  3. landscape: a terrain made on A, sculpted on A and on B, read back with
     terrain_heightmap on the other side;
  4. HorizonCode: Level Script and Game Instance (the delta-only documents),
     a variable, a node, a node move, each from a different editor;
  5. material: B (a client) creates a master and sets a parameter, A reads it;
     then A sets one and B reads it;
  6. late join: C joins after all of that and has every entity, the sculpted
     heights, both HC graphs and the material parameter;
  7. rejoin: B leaves, A changes an entity, the landscape, the Game Instance
     and the material, B joins again with the same code and has all of it;
  8. save and idle: B saves the scene → the host logs `session state kept`
     and A's own scene file holds the session's entities; then nobody edits
     for > 5 s → a second `session state kept` (idle) and the file follows;
  9. landscape heights byte for byte: every editor saves, the three saved
     `sculptHeightsB64` fields of the terrain are compared;
 10. at the end all three hold the same content uuids.

A refusal that the audit records as a known limit is logged as `known:` and is
not a failure; anything else that does not arrive is.

Usage:
    scripts/he_collab_full_pass.py OUTDIR [--editor PATH] [--timeout S] [--team-server]

--team-server runs the whole pass in a project whose Config/ProjectSettings.json
says "sourceControl": {"mode": "teamServer"} (topic 199's opt-in, step 9): the
session must sync and settle the same way while git is handed to the server.
"""

import base64
import json
import os
import pathlib
import shutil
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import he_mcp_multiclient as mc  # noqa: E402
import he_collab_two_editors as two  # noqa: E402  — Peer, wait_for, close, same_content

log = mc.log
Peer, wait_for, close = two.Peer, two.wait_for, two.close
SCENE_REL = pathlib.Path("Content") / "StartupScene.hescene"
SETTLE_LINE = "Collab: session state kept"


# Children every editor builds for itself from a replicated parent, like the
# built-ins: a landscape's render chunks (isTerrainGenerated, never published by
# syncStructuralChanges). Their uuids differ per process by design, so they are
# compared by name and count, the way two.same_content treats the built-ins.
GENERATED = {"TerrainChunk"}


def same_content(a, b, when, expect, transcript):
    ents_a, ents_b = a.entities(), b.entities()

    def split(ents):
        content = {u for u, e in ents.items() if not e.get("builtin") and e.get("name") not in GENERATED}
        local = sorted(e.get("name") for e in ents.values() if e.get("builtin") or e.get("name") in GENERATED)
        return content, local
    content_a, local_a = split(ents_a)
    content_b, local_b = split(ents_b)
    only_a, only_b = content_a - content_b, content_b - content_a
    for tag, only, ents in (("A", only_a, ents_a), ("B", only_b, ents_b)):
        for u in sorted(only):
            log("    only on %s: %s %s %s" % (tag, u, ents[u].get("name"), ents[u].get("components")))
    expect(not only_a and not only_b, "%s: both hold the same %d content uuids (only %s: %d, only %s: %d)"
           % (when, len(content_a), a.tag, len(only_a), b.tag, len(only_b)))
    expect(local_a == local_b, "%s: same per-editor built-ins/generated children by name %s" % (when, local_a))
    transcript.setdefault("content", {})[when] = {"content_uuids": len(content_a), "local": local_a,
                                                  "only_a": sorted(only_a), "only_b": sorted(only_b)}


def comps(peer, uuid):
    ok, sc = peer.raw("entity_get", {"uuid": uuid})
    return sc.get("components", {}) if ok else {}


def height_at(peer, uuid, x, z):
    ok, sc = peer.raw("terrain_heightmap", {"uuid": uuid, "minX": x - 0.5, "maxX": x + 0.5,
                                            "minZ": z - 0.5, "maxZ": z + 0.5})
    hs = sc.get("heights") if ok else None
    return hs[len(hs) // 2] if hs else None


def hc_doc(peer, key):
    ok, sc = peer.raw("hc_get", {"key": key})
    return sc if ok else {}


def hc_var_names(peer, key):
    return sorted(v.get("name") for v in hc_doc(peer, key).get("variables", []))


def hc_node_pos(peer, key, node_id):
    for n in hc_doc(peer, key).get("nodes", []):
        if n.get("id") == node_id:
            return n.get("pos")
    return None


def mat_param(peer, path, name):
    ok, sc = peer.raw("material_info", {"path": path})
    if not ok:
        return None
    for p in sc.get("params", []):
        if p.get("name") == name:
            return p.get("value")
    return None


def scene_entity(path, name):
    """The entity called `name` in a saved scene file, or None."""
    try:
        data = json.loads(pathlib.Path(path).read_text())
    except (OSError, ValueError):
        return None
    for e in data.get("entities", []):
        if e.get("name") == name:
            return e
    return None


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    editor_path = mc.EDITOR
    timeout = int(os.environ.get("HE_SHOT_TIMEOUT", "1200"))
    team = "--team-server" in args
    for i, a in enumerate(args):
        if a == "--editor":
            editor_path = pathlib.Path(args[i + 1])
        if a == "--timeout":
            timeout = int(args[i + 1])
    if not editor_path.exists():
        log("no editor at %s" % editor_path)
        return 2
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)

    os.environ["HE_MCP_COLLAB_CONTROL"] = "1"
    os.environ["HE_COLLAB_OFFLINE"] = "1"

    if team:
        # mc.Editor copies mc.PROJECT_SRC when it starts the process, so the
        # opt-in has to be in the source copy, not patched in afterwards.
        src = pathlib.Path(tempfile.mkdtemp(prefix="he_collab_team_")) / "HorizonTutorial"
        shutil.copytree(mc.PROJECT_SRC, src)
        (src / "Config").mkdir(exist_ok=True)
        (src / "Config" / "ProjectSettings.json").write_text(json.dumps(
            {"sourceControl": {"mode": "teamServer", "team": "t", "project": "p"}}, indent=1))
        mc.PROJECT_SRC = src
        log("team-server mode: project source %s" % src)

    failures, known = [], []
    transcript = {"team_server": team}

    def expect(cond, what):
        mc.expect(bool(cond), what, failures)

    def note_known(what):
        log("  known %s" % what)
        known.append(what)

    peers = []
    try:
        a = Peer("A", outdir / "a", editor_path, timeout)
        b = Peer("B", outdir / "b", editor_path, timeout)
        c = Peer("C", outdir / "c", editor_path, timeout)
        peers = [a, b, c]
        for p in peers:
            p.connect()
            expect(p.call("scene_info").get("sceneOpen"), "%s has a scene open" % p.tag)
        if failures:
            return 1

        # ── Host + first join ────────────────────────────────────────────────
        host = a.call("collab_host", {"name": "Host A"})
        expect(host.get("status") == "hosting", "A is hosting")

        def join(peer, name):
            peer.call("collab_join", {"host": "127.0.0.1", "port": host["port"],
                                      "joinCode": host["joinCode"], "name": name})
            st, dt = wait_for("%s joined" % peer.tag,
                              lambda: (lambda s: s if s.get("status") in ("joined", "failed") else None)(peer.status()))
            ok = bool(st) and st.get("status") == "joined"
            expect(ok, "%s reached `joined` (%.1fs)%s" % (peer.tag, dt, "" if ok else " %s" % (st or {}).get("error")))
            return ok

        if not join(b, "Guest B"):
            return 1

        # ── 1. Entities ──────────────────────────────────────────────────────
        parent = a.call("entity_create", {"name": "SyncParent", "position": [0.0, 0.0, 0.0]})["uuid"]
        child = a.call("entity_create", {"name": "SyncChild", "position": [2.0, 1.0, 0.0]})["uuid"]
        lamp = a.call("entity_create", {"name": "SyncLamp", "position": [0.0, 4.0, 0.0],
                                        "components": {"light": {"type": 1, "intensity": 2.0,
                                                                 "range": 8.0}}})["uuid"]
        got, dt = wait_for("B sees A's three entities",
                           lambda: {parent, child, lamp} <= set(b.entities()))
        expect(got, "entities: A creates three → B has all three, same uuids (%.1fs)" % dt)

        ok, sc, pend = b.retry_lock("entity_set_transform",
                                    {"uuid": child, "position": [3.0, 1.0, -1.0], "scale": [2.0, 3.0, 4.0]})
        expect(ok, "entities: B moves + scales SyncChild (%s, %d pending)" % (sc.get("code", "ok"), pend))
        got, dt = wait_for("A sees B's scale",
                           lambda: close(comps(a, child).get("transform", {}).get("scale"), [2.0, 3.0, 4.0]))
        expect(got, "entities: A sees B's scale [2,3,4] (%.1fs)" % dt)
        expect(close(two.world_pos(a, child), [3.0, 1.0, -1.0]), "entities: A sees B's move %s" % two.world_pos(a, child))

        ok, sc, pend = b.retry_lock("entity_set_components",
                                    {"uuid": lamp, "patch": {"light": {"intensity": 9.5}, "__name": "SyncLampB"}})
        expect(ok, "entities: B sets the lamp's intensity and name (%s)" % sc.get("code", "ok"))
        got, dt = wait_for("A sees intensity 9.5",
                           lambda: abs(comps(a, lamp).get("light", {}).get("intensity", 0) - 9.5) < 1e-4)
        expect(got, "entities: A sees B's property (light.intensity 9.5) (%.1fs)" % dt)
        expect(a.entities().get(lamp, {}).get("name") == "SyncLampB", "entities: A sees B's rename")

        # B's client holds the lamp since the patch, so a reparent from A is a
        # lock refusal by design; B reparents, A reads it.
        ok, sc = a.raw("entity_reparent", {"uuid": lamp, "parent": parent})
        expect(not ok and sc.get("code") == "locked_by_other",
               "entities: A's reparent of the B-held lamp is refused: %s" % (sc.get("code") if not ok else "ACCEPTED"))
        ok, sc, _ = b.retry_lock("entity_reparent", {"uuid": lamp, "parent": parent})
        expect(ok, "entities: B reparents the lamp under SyncParent (%s)" % sc.get("code", "ok"))
        got, dt = wait_for("A sees the reparent", lambda: a.entities().get(lamp, {}).get("parent") == parent)
        expect(got, "entities: A sees B's reparent (%.1fs)" % dt)
        expect(close(two.world_pos(a, lamp), [0.0, 4.0, 0.0]), "entities: the lamp kept its world position on A %s"
               % two.world_pos(a, lamp))

        doomed = a.call("entity_create", {"name": "SyncDoomed", "position": [0.0, 0.0, 5.0]})["uuid"]
        wait_for("B sees SyncDoomed", lambda: doomed in b.entities())
        ok, sc, _ = b.retry_lock("entity_destroy", {"uuid": doomed})
        expect(ok, "entities: B deletes SyncDoomed (%s)" % sc.get("code", "ok"))
        got, dt = wait_for("A loses SyncDoomed", lambda: doomed not in a.entities())
        expect(got, "entities: B's delete reached A (%.1fs)" % dt)

        # ── 2. A peer-held entity is not deleted or renamed behind its back ──
        # B's client still holds SyncChild from the move above.
        ok, sc = a.raw("entity_destroy", {"uuid": child})
        expect(not ok and sc.get("code") == "locked_by_other",
               "conflict: A's delete of B-held SyncChild is refused: %s" % (sc.get("code") if not ok else "ACCEPTED"))
        ok, sc = a.raw("entity_set_components", {"uuid": child, "patch": {"__name": "Stolen"}})
        expect(not ok and sc.get("code") == "locked_by_other",
               "conflict: A's rename of B-held SyncChild is refused: %s" % (sc.get("code") if not ok else "ACCEPTED"))
        time.sleep(1.0)
        expect(b.entities().get(child, {}).get("name") == "SyncChild", "conflict: B still has SyncChild, unrenamed")

        # ── 3. Landscape ─────────────────────────────────────────────────────
        land = a.call("entity_create", {"name": "SyncLand", "components": {
            "terrain": {"sizeX": 64.0, "sizeZ": 64.0, "resolution": 65}}})["uuid"]
        got, dt = wait_for("B sees SyncLand", lambda: land in b.entities())
        expect(got, "landscape: B has A's new terrain (%.1fs)" % dt)
        # The MCP terrain tools write through EditorCommands (Origin::External)
        # and so take the entity lock; the human brush does not (TerrainSync).
        ok, r, _ = a.retry_lock("terrain_sculpt", {"uuid": land, "x": 0.0, "z": 0.0, "op": "set", "height": 7.0,
                                                   "radius": 6.0, "falloff": 0.0})
        expect(ok and r.get("changed", 0) > 0, "landscape: A sculpts height 7 at (0,0) (%s, %d samples)"
               % (r.get("code", "ok"), r.get("changed", 0)))
        got, dt = wait_for("B sees height 7", lambda: abs((height_at(b, land, 0, 0) or 0) - 7.0) < 1e-3)
        expect(got, "landscape: B reads height 7 at (0,0) (%.1fs): %s" % (dt, height_at(b, land, 0, 0)))
        a.reconnect()   # A's client lets go of the terrain lock (McpLockBook)
        ok, sc, pend = b.retry_lock("terrain_sculpt", {"uuid": land, "x": -16.0, "z": 16.0, "op": "set",
                                                       "height": 4.0, "radius": 5.0, "falloff": 0.0})
        t0 = time.time()
        while not ok and sc.get("code") == "locked_by_other" and time.time() < t0 + two.SYNC_TIMEOUT:
            time.sleep(0.2)
            ok, sc, _ = b.retry_lock("terrain_sculpt", {"uuid": land, "x": -16.0, "z": 16.0, "op": "set",
                                                        "height": 4.0, "radius": 5.0, "falloff": 0.0})
        expect(ok, "landscape: B sculpts height 4 at (-16,16) (%s, %.1fs)" % (sc.get("code", "ok"), time.time() - t0))
        got, dt = wait_for("A sees height 4", lambda: abs((height_at(a, land, -16, 16) or 0) - 4.0) < 1e-3)
        expect(got, "landscape: A reads B's height 4 at (-16,16) (%.1fs): %s" % (dt, height_at(a, land, -16, 16)))
        expect(abs((height_at(b, land, 0, 0) or 0) - 7.0) < 1e-3, "landscape: B's stroke kept A's height 7 on B")
        expect(abs((height_at(a, land, 0, 0) or 0) - 7.0) < 1e-3, "landscape: B's stroke kept A's height 7 on A")

        # ── 4. HorizonCode: Level Script + Game Instance ─────────────────────
        ok, sc = a.raw("hc_add_variable", {"key": "levelscript", "name": "Score", "type": "Int"})
        expect(ok, "hc: A adds variable Score to the Level Script (%s)" % sc.get("code", "ok"))
        ok, sc = a.raw("hc_add_node", {"key": "levelscript", "type": "Branch", "position": [100, 40]})
        ls_node = sc.get("node", {}).get("id") if ok else None
        expect(ok, "hc: A adds a Branch node to the Level Script (id %s)" % ls_node)
        got, dt = wait_for("B sees Score", lambda: "Score" in hc_var_names(b, "levelscript"))
        expect(got, "hc: B sees the Level Script variable Score (%.1fs)" % dt)
        got, dt = wait_for("B sees the node", lambda: hc_node_pos(b, "levelscript", ls_node) is not None)
        expect(got, "hc: B sees the Level Script node %s (%.1fs)" % (ls_node, dt))
        node = next((dict(n) for n in hc_doc(a, "levelscript").get("nodes", []) if n.get("id") == ls_node), {})
        node.pop("pins", None)
        node["pos"] = [300.0, 80.0]
        ok, sc = a.raw("hc_set_node", {"key": "levelscript", "node": node})
        expect(ok, "hc: A moves the node to (300,80) (%s)" % sc.get("code", "ok"))
        got, dt = wait_for("B sees the move", lambda: close(hc_node_pos(b, "levelscript", ls_node), [300.0, 80.0]))
        expect(got, "hc: B sees the node move (%.1fs): %s" % (dt, hc_node_pos(b, "levelscript", ls_node)))

        ok, sc = b.raw("hc_add_variable", {"key": "gameinstance", "name": "Lives", "type": "Int"})
        expect(ok, "hc: B adds variable Lives to the Game Instance (%s %s)" % (sc.get("code", "ok"), sc.get("message", "")))
        ok, sc = b.raw("hc_add_node", {"key": "gameinstance", "type": "Branch", "position": [40, 200]})
        gi_node = sc.get("node", {}).get("id") if ok else None
        expect(ok, "hc: B adds a node to the Game Instance (id %s)" % gi_node)
        got, dt = wait_for("A sees Lives", lambda: "Lives" in hc_var_names(a, "gameinstance"))
        expect(got, "hc: A sees B's Game Instance variable Lives (%.1fs)" % dt)
        got, dt = wait_for("A sees B's node", lambda: hc_node_pos(a, "gameinstance", gi_node) is not None)
        expect(got, "hc: A sees B's Game Instance node (%.1fs)" % dt)

        # ── 5. Material ──────────────────────────────────────────────────────
        # The client creates it: on the HOST publishAsset grants itself the
        # lock and keeps it (reconnect audit, Traps), which would lock B out.
        mat = "Materials/CollabMat.hasset"
        ok, sc, pend = b.retry_lock("material_create", {"path": "Materials/CollabMat"})
        expect(ok, "material: B creates %s (%s, %d pending)" % (mat, sc.get("code", "ok"), pend))
        got, dt = wait_for("A has the material", lambda: mat_param(a, mat, "Roughness") is not None)
        expect(got, "material: A has B's new material (%.1fs)" % dt)
        ok, sc, pend = b.retry_lock("material_set_param", {"path": mat, "name": "Roughness", "value": 0.25})
        expect(ok, "material: B sets Roughness 0.25 (%s %s, %d pending)" % (sc.get("code", "ok"), sc.get("message", ""), pend))
        got, dt = wait_for("A sees Roughness 0.25", lambda: abs((mat_param(a, mat, "Roughness") or 0) - 0.25) < 1e-4)
        expect(got, "material: A sees B's Roughness 0.25 (%.1fs): %s" % (dt, mat_param(a, mat, "Roughness")))
        ok, sc, pend = a.retry_lock("material_set_param", {"path": mat, "name": "Metallic", "value": 0.75})
        transcript["host_material_edit_while_client_created"] = {"ok": ok, "code": sc.get("code"), "message": sc.get("message")}
        if ok:
            got, dt = wait_for("B sees Metallic 0.75", lambda: abs((mat_param(b, mat, "Metallic") or 0) - 0.75) < 1e-4)
            expect(got, "material: B sees A's Metallic 0.75 (%.1fs): %s" % (dt, mat_param(b, mat, "Metallic")))
            mat_writer = a
        else:
            note_known("material: A's edit of B's material refused while B's editor keeps the lock (%s)" % sc.get("code"))
            mat_writer = b

        # ── 6. Late join ─────────────────────────────────────────────────────
        if join(c, "Late C"):
            st, _ = wait_for("roster 3", lambda: (lambda s: s if len(s.get("participants", [])) == 3 else None)(a.status()))
            expect(st, "late join: A's roster has three people")
            same_content(a, c, "late join: A vs C", expect, transcript)
            expect(close(comps(c, child).get("transform", {}).get("scale"), [2.0, 3.0, 4.0]), "late join: C has B's scale")
            expect(abs(comps(c, lamp).get("light", {}).get("intensity", 0) - 9.5) < 1e-4, "late join: C has B's intensity")
            expect(c.entities().get(lamp, {}).get("parent") == parent, "late join: C has the reparent")
            got, dt = wait_for("C's height 7", lambda: abs((height_at(c, land, 0, 0) or 0) - 7.0) < 1e-3)
            expect(got, "late join: C reads A's height 7 (%.1fs): %s" % (dt, height_at(c, land, 0, 0)))
            expect(abs((height_at(c, land, -16, 16) or 0) - 4.0) < 1e-3,
                   "late join: C reads B's height 4: %s" % height_at(c, land, -16, 16))
            got, dt = wait_for("C's HC", lambda: "Score" in hc_var_names(c, "levelscript")
                               and "Lives" in hc_var_names(c, "gameinstance"))
            expect(got, "late join: C has Score (Level Script) and Lives (Game Instance) (%.1fs): %s / %s"
                   % (dt, hc_var_names(c, "levelscript"), hc_var_names(c, "gameinstance")))
            expect(close(hc_node_pos(c, "levelscript", ls_node), [300.0, 80.0]),
                   "late join: C has the moved Level Script node %s" % hc_node_pos(c, "levelscript", ls_node))
            got, dt = wait_for("C's material", lambda: abs((mat_param(c, mat, "Roughness") or 0) - 0.25) < 1e-4)
            expect(got, "late join: C has the material with Roughness 0.25 (%.1fs): %s" % (dt, mat_param(c, mat, "Roughness")))

        # ── 7. Rejoin: B is away while A changes things ──────────────────────
        left = b.call("collab_leave")
        expect(left.get("inSession") is False, "rejoin: B left the session")
        wait_for("A's roster drops B", lambda: len(a.status().get("participants", [])) == 2)
        ok, sc, _ = a.retry_lock("entity_set_transform", {"uuid": parent, "position": [5.0, 0.0, 5.0]})
        expect(ok, "rejoin: A moves SyncParent while B is away (%s)" % sc.get("code", "ok"))
        # B's client held the terrain; B leaving the session hands it back.
        t0 = time.time()
        while True:
            ok, sc, _ = a.retry_lock("terrain_sculpt", {"uuid": land, "x": 16.0, "z": -16.0, "op": "set",
                                                        "height": 3.0, "radius": 5.0, "falloff": 0.0})
            if ok or sc.get("code") != "locked_by_other" or time.time() > t0 + two.SYNC_TIMEOUT:
                break
            time.sleep(0.2)
        expect(ok, "rejoin: A sculpts height 3 at (16,-16) while B is away (%s, %.1fs)"
               % (sc.get("code", "ok"), time.time() - t0))
        ok, sc = a.raw("hc_add_variable", {"key": "gameinstance", "name": "WhileAway", "type": "Bool"})
        expect(ok, "rejoin: A adds WhileAway to the Game Instance (%s %s)" % (sc.get("code", "ok"), sc.get("message", "")))
        away_mat = mat_writer is a
        if away_mat:
            ok, sc, _ = a.retry_lock("material_set_param", {"path": mat, "name": "Specular", "value": 0.1})
            expect(ok, "rejoin: A sets Specular 0.1 while B is away (%s)" % sc.get("code", "ok"))
        if join(b, "Guest B"):
            got, dt = wait_for("B sees the move", lambda: close(two.world_pos(b, parent), [5.0, 0.0, 5.0]))
            expect(got, "rejoin: B has A's move from while it was away (%.1fs)" % dt)
            got, dt = wait_for("B height 3", lambda: abs((height_at(b, land, 16, -16) or 0) - 3.0) < 1e-3)
            expect(got, "rejoin: B reads height 3 from while it was away (%.1fs): %s" % (dt, height_at(b, land, 16, -16)))
            got, dt = wait_for("B WhileAway", lambda: "WhileAway" in hc_var_names(b, "gameinstance"))
            expect(got, "rejoin: B has WhileAway in the Game Instance (%.1fs): %s" % (dt, hc_var_names(b, "gameinstance")))
            if away_mat:
                got, dt = wait_for("B Specular", lambda: abs((mat_param(b, mat, "Specular") or 0) - 0.1) < 1e-4)
                expect(got, "rejoin: B has Specular 0.1 (%.1fs): %s" % (dt, mat_param(b, mat, "Specular")))
            got, dt = wait_for("C saw it live", lambda: "WhileAway" in hc_var_names(c, "gameinstance"))
            expect(got, "rejoin: C (stayed in) saw WhileAway live (%.1fs)" % dt)

        # ── 8. Save and idle on the host ─────────────────────────────────────
        # Watched on A's scene FILE: the editor's log reaches its file through a
        # buffered stdout, often seconds late, so a count of log lines taken
        # mid-run says nothing. The log lines are checked after shutdown.
        a_scene = a.ed.outdir / "project" / SCENE_REL
        mtime = lambda: a_scene.stat().st_mtime if a_scene.exists() else 0.0  # noqa: E731
        time.sleep(6.0)   # let the rejoin edits settle as idle first, so the next write is the save
        m0 = mtime()
        b.call("scene_save")
        got, dt = wait_for("A writes its scene after B's save", lambda: mtime() > m0, timeout=15)
        expect(got and dt < 4.5, "save: B's scene_save → A writes its own scene file (%.1fs, under the 5 s idle)" % dt)
        expect(scene_entity(a_scene, "SyncChild") is not None, "save: A's own scene file holds SyncChild")
        marker = c.call("entity_create", {"name": "IdleMarker", "position": [0.0, 0.0, -6.0]})["uuid"]
        wait_for("A sees IdleMarker", lambda: marker in a.entities())
        t_edit = time.time()
        got, dt = wait_for("IdleMarker in A's file", lambda: scene_entity(a_scene, "IdleMarker") is not None, timeout=20)
        expect(got and dt >= 3.5,
               "idle: C's IdleMarker reaches A's scene file once nobody edits (%.1fs after the edit)" % dt)
        transcript["save_idle_seconds"] = {"idle_after_edit": round(dt, 2)}

        # ── 9. Landscape heights byte for byte ───────────────────────────────
        blobs = {}
        for p in peers:
            p.call("scene_save")
            e = scene_entity(p.ed.outdir / "project" / SCENE_REL, "SyncLand")
            blobs[p.tag] = ((e or {}).get("components", {}).get("terrain", {}) or {}).get("sculptHeightsB64")
        sizes = {k: (len(base64.b64decode(v)) if v else 0) for k, v in blobs.items()}
        log("    saved sculptHeights bytes: %s" % sizes)
        expect(blobs["A"] and blobs["A"] == blobs["B"] == blobs["C"],
               "landscape: the saved height grids of A, B and C are byte-identical (%s)" % sizes)

        # ── 10. Same content everywhere, then leave ──────────────────────────
        same_content(a, b, "end: A vs B", expect, transcript)
        same_content(a, c, "end: A vs C", expect, transcript)
        for p in (c, b, a):
            p.call("collab_leave")
    except Exception as exc:  # a crash is a failure with its reason, not a traceback
        log("ERROR: %s" % exc)
        failures.append("exception: %s" % exc)
    finally:
        for p in peers:
            if p.client:
                p.client.close()
            p.ed.stop()
            # After stop: the process flushed its stdout on the way out.
            text = (p.ed.outdir / "editor.log").read_bytes().decode("utf-8", "replace")
            lines = [l for l in text.splitlines() if "Collab" in l or "MCP: collab" in l]
            (outdir / ("collab_log_%s.txt" % p.tag.lower())).write_text("\n".join(lines))
            log("%s editor log: %d collab lines" % (p.tag, len(lines)))
            if p.tag == "A" and not any(f.startswith("exception") for f in failures):
                kept = [l for l in lines if SETTLE_LINE in l and "::Scene::" in l]
                expect(any("(saved)" in l for l in kept), "save: A's log has `%s: ::Scene:: (saved)`" % SETTLE_LINE)
                expect(any("(idle)" in l for l in kept), "idle: A's log has `%s: ::Scene:: (idle)`" % SETTLE_LINE)
                refused = [l for l in text.splitlines() if "does not hold lock" in l]
                for l in refused:
                    log("    " + l.strip())
                expect(not refused, "no save of anyone's was refused by the host for a lock (%d)" % len(refused))
        transcript["known"] = known
        (outdir / "transcript.json").write_text(json.dumps(transcript, indent=1, default=str))

    log("RESULT: %s (%d failures, %d known limits)" % ("PASS" if not failures else "FAIL",
                                                      len(failures), len(known)))
    for f in failures:
        log("  - " + f)
    for k in known:
        log("  known: " + k)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
