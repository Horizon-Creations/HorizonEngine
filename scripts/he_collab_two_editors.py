#!/usr/bin/env python3
"""he_collab_two_editors — two real editor processes in one collaboration session.

The Collaboration layer is covered by tests that put two CollabControllers in
ONE process (tests/test_collab_controller.cpp). This is the run those tests
cannot be: two deployed HorizonEditor processes, each with its own HOME, its
own copy of the project and its own MCP bridge, one hosting and one joining over
real loopback TCP + SecureTransport, and every sync checked from the OTHER
process through that process's own tools (Thema 86, Schritt 3).

Host and join go through `collab_host` / `collab_join`, which only exist when
the editor runs with HE_MCP_COLLAB_CONTROL=1 and call the same CollabController
methods the panel's buttons do. HE_COLLAB_OFFLINE=1 keeps the router, the LAN
beacon and the public directory out of it.

What it checks, each from the side that did NOT make the change:
  1. both editors boot with the project open and list the collab tools;
  2. pre-join state differs: a marker only A has, a marker only B has;
  3. A hosts, B joins by address + code, B reaches `joined`, both rosters name
     both people;
  4. the snapshot REPLACED B's world: B has A's marker under A's uuid, B's own
     marker is gone, and both uuid sets are identical;
  5. A creates an entity → B sees it, same uuid, same position;
  6. B moves it (lock requested from the host, lock_pending retried) → A sees it;
  7. while B holds that lock, A's move is refused `locked_by_other`; B's client
     hangs up, the lock goes back, A's move lands and B sees it;
  8. B creates and renames an entity → A sees the name;
  9. deletes both ways → gone on the other side;
 10. B leaves → A's roster shrinks back to A.

Usage:
    scripts/he_collab_two_editors.py OUTDIR [--editor PATH] [--timeout S]

Both editors boot in parallel (a Debug editor needs ~6.5 min to its endpoint
file on macOS 27); the run prints `RESULT: PASS|FAIL` last and writes both
editor logs and a JSON transcript into OUTDIR.
"""

import json
import os
import pathlib
import shutil
import socket
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import he_mcp_multiclient as mc  # noqa: E402  — Editor + Client: same boot, same handshake

log = mc.log

# How long a sync may take to show up on the other side. The session pumps once
# per editor frame and the hidden editors are paced, so this is generous on
# purpose: a real failure is a value that NEVER arrives, not a slow one.
SYNC_TIMEOUT = 30.0


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Peer:
    """One editor process plus the MCP client that drives it."""

    def __init__(self, tag, outdir, editor_path, timeout):
        self.tag = tag
        self.ed = mc.Editor(outdir, free_port(), editor_path, timeout)
        self.client = None

    def connect(self):
        self.ed.wait_endpoint()
        self.client = mc.Client(self.tag, self.ed.endpoint)

    def reconnect(self):
        if self.client:
            self.client.close()
        self.client = mc.Client(self.tag, self.ed.endpoint)

    def raw(self, tool, args=None):
        """(ok, structuredContent) — refusals are answers here, not exceptions."""
        reply = self.client.bridge.request("tools/call",
                                           {"name": tool, "arguments": args or {}})
        if "error" in reply:
            raise RuntimeError("%s: %s → rpc error %s" % (self.tag, tool, reply["error"]))
        res = reply["result"]
        sc = res.get("structuredContent") or {}
        return (not res.get("isError")), sc

    def call(self, tool, args=None):
        ok, sc = self.raw(tool, args)
        if not ok:
            raise RuntimeError("%s: %s → %s: %s" % (self.tag, tool, sc.get("code"), sc.get("message")))
        return sc

    def retry_lock(self, tool, args, timeout=SYNC_TIMEOUT):
        """Call a mutating tool, retrying while the host's lock answer is in flight."""
        deadline = time.time() + timeout
        pending = 0
        while True:
            ok, sc = self.raw(tool, args)
            if ok:
                return True, sc, pending
            if sc.get("code") != "lock_pending" or time.time() > deadline:
                return False, sc, pending
            pending += 1
            time.sleep(0.1)

    def entities(self):
        return {e["uuid"]: e for e in self.call("entity_list").get("entities", [])}

    def by_name(self, name):
        return [e for e in self.entities().values() if e.get("name") == name]

    def status(self):
        return self.call("collab_status")


def wait_for(what, fn, timeout=SYNC_TIMEOUT, interval=0.2):
    """Poll `fn` until it returns something truthy; (value, seconds) or (None, timeout)."""
    t0 = time.time()
    while True:
        v = fn()
        if v:
            return v, time.time() - t0
        if time.time() - t0 > timeout:
            log("  waited %.0fs for %s — never happened" % (timeout, what))
            return None, time.time() - t0
        time.sleep(interval)


def close(a, b, eps=1e-3):
    return a is not None and b is not None and len(a) == len(b) and \
        all(abs(x - y) < eps for x, y in zip(a, b))


def world_pos(peer, uuid):
    ok, sc = peer.raw("entity_get", {"uuid": uuid})
    return sc.get("worldPosition") if ok else None


def roster_names(status):
    return sorted(p.get("name") for p in status.get("participants", []))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    editor_path = mc.EDITOR
    timeout = int(os.environ.get("HE_SHOT_TIMEOUT", "1200"))
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

    # Read by mc.Editor through os.environ: the tools that host and join exist
    # only in a run that asks for them, and nothing of this run leaves the box.
    os.environ["HE_MCP_COLLAB_CONTROL"] = "1"
    os.environ["HE_COLLAB_OFFLINE"] = "1"

    failures = []
    transcript = {}

    def expect(cond, what):
        mc.expect(bool(cond), what, failures)

    a = b = None
    try:
        # Both boot at once: two Debug editors one after the other would be
        # a quarter of an hour before the first check.
        a = Peer("A", outdir / "a", editor_path, timeout)
        b = Peer("B", outdir / "b", editor_path, timeout)
        t0 = time.time()
        a.connect()
        b.connect()
        log("both endpoints up after %.0fs" % (time.time() - t0))

        # ── 1. Both editors are up with the project and the tools ────────────
        for p in (a, b):
            names = [t["name"] for t in p.client.bridge.request("tools/list")
                     .get("result", {}).get("tools", [])]
            info = p.call("scene_info")
            log("%s: project=%s scene=%s entities=%s inSession=%s"
                % (p.tag, info.get("project"), info.get("scene"),
                   info.get("entityCount"), info.get("inSession")))
            expect(info.get("sceneOpen"), "%s has a scene open" % p.tag)
            expect(all(n in names for n in ("collab_status", "collab_host", "collab_join",
                                            "collab_leave")),
                   "%s lists collab_status/host/join/leave" % p.tag)
            expect(p.status().get("status") == "idle", "%s starts idle" % p.tag)
        if failures:
            return 1

        # ── 2. Different worlds before the join ──────────────────────────────
        pre_a = a.call("entity_create", {"name": "PreJoinMarkerA", "position": [3.0, 0.5, -2.0]})
        pre_b = b.call("entity_create", {"name": "GuestLocalOnlyB", "position": [-3.0, 0.5, 2.0]})
        expect(not b.by_name("PreJoinMarkerA"), "before the join B does not have A's marker")

        # ── 3. Host, join, rosters ───────────────────────────────────────────
        host = a.call("collab_host", {"name": "Host A"})
        transcript["collab_host"] = {k: v for k, v in host.items() if k != "joinCode"}
        log("A hosting: %s:%s (code %d chars)" % (host.get("address"), host.get("port"),
                                                  len(host.get("joinCode", ""))))
        expect(host.get("status") == "hosting", "A is hosting")
        join = b.call("collab_join", {"host": "127.0.0.1", "port": host["port"],
                                      "joinCode": host["joinCode"], "name": "Guest B"})
        expect(join.get("status") in ("connecting", "joined"), "B's join started (%s)" % join.get("status"))

        def b_joined():
            st = b.status()
            if st.get("status") == "failed":
                log("  B failed: %s" % st.get("error"))
                return st
            return st if st.get("status") == "joined" else None
        st_b, dt = wait_for("B joined", b_joined)
        expect(st_b and st_b.get("status") == "joined", "B reached `joined` (%.1fs)" % dt)
        if not st_b or st_b.get("status") != "joined":
            return 1
        st_a, _ = wait_for("A's roster has 2", lambda: (lambda s: s if len(s.get("participants", [])) == 2 else None)(a.status()))
        st_b = b.status()
        transcript["roster_a"], transcript["roster_b"] = st_a, st_b
        want = ["Guest B", "Host A"]
        expect(st_a and roster_names(st_a) == want, "A's roster names both: %s" % (st_a and roster_names(st_a)))
        expect(roster_names(st_b) == want, "B's roster names both: %s" % roster_names(st_b))
        expect(st_a and st_a.get("you") != st_b.get("you"), "A and B are different participants")

        # ── 4. The snapshot replaced B's world ───────────────────────────────
        ents_a, ents_b = a.entities(), b.entities()
        expect(pre_a["uuid"] in ents_b, "B has A's pre-join marker under A's uuid (snapshot)")
        expect(pre_b["uuid"] not in ents_b and not b.by_name("GuestLocalOnlyB"),
               "B's own pre-join marker is gone (world replaced, not merged)")
        only_a = set(ents_a) - set(ents_b)
        only_b = set(ents_b) - set(ents_a)
        expect(not only_a and not only_b,
               "A and B hold the same %d uuids (only A: %d, only B: %d)"
               % (len(ents_a), len(only_a), len(only_b)))

        # ── 5. A creates → B sees ────────────────────────────────────────────
        live_a = a.call("entity_create", {"name": "LiveFromA", "position": [1.0, 2.0, 3.0]})
        ua = live_a["uuid"]
        got, dt = wait_for("B sees LiveFromA", lambda: ua in b.entities())
        expect(got, "B sees A's new entity under the same uuid (%.1fs)" % dt)
        expect(close(world_pos(b, ua), [1.0, 2.0, 3.0]), "…at A's position %s" % world_pos(b, ua))

        # ── 6. B moves → A sees ──────────────────────────────────────────────
        ok, sc, pend = b.retry_lock("entity_set_transform", {"uuid": ua, "position": [-4.0, 1.0, 2.0]})
        expect(ok, "B's move went through after %d lock_pending retries (%s)" % (pend, sc.get("code", "ok")))
        got, dt = wait_for("A sees B's move", lambda: close(world_pos(a, ua), [-4.0, 1.0, 2.0]))
        expect(got, "A sees B's move (%.1fs): %s" % (dt, world_pos(a, ua)))

        # ── 7. The lock holds across processes, and is handed back ───────────
        ok, sc = a.raw("entity_set_transform", {"uuid": ua, "position": [9.0, 9.0, 9.0]})
        expect(not ok and sc.get("code") == "locked_by_other",
               "A's move of B's entity is refused: %s" % (sc.get("code") if not ok else "ACCEPTED"))
        expect(close(world_pos(a, ua), [-4.0, 1.0, 2.0]), "…and did not move it on A")
        b.reconnect()   # the client that held the lock hangs up → its locks go back
        ok, sc, pend = False, {}, 0
        deadline = time.time() + SYNC_TIMEOUT
        while time.time() < deadline:
            ok, sc, p = a.retry_lock("entity_set_transform", {"uuid": ua, "position": [5.0, 0.0, 5.0]})
            pend += p
            if ok or sc.get("code") != "locked_by_other":
                break
            time.sleep(0.2)
        expect(ok, "after B's client left, A's move lands (%s, %d pending)" % (sc.get("code", "ok"), pend))
        got, dt = wait_for("B sees A's move", lambda: close(world_pos(b, ua), [5.0, 0.0, 5.0]))
        expect(got, "B sees A's move (%.1fs): %s" % (dt, world_pos(b, ua)))

        # ── 8. B creates + renames → A sees the name ─────────────────────────
        live_b = b.call("entity_create", {"name": "LiveFromB", "position": [0.0, 3.0, 0.0]})
        ub = live_b["uuid"]
        got, dt = wait_for("A sees LiveFromB", lambda: ub in a.entities())
        expect(got, "A sees B's new entity under the same uuid (%.1fs)" % dt)
        ok, sc, pend = b.retry_lock("entity_set_components", {"uuid": ub, "patch": {"__name": "RenamedByB"}})
        expect(ok, "B's rename went through (%s, %d pending)" % (sc.get("code", "ok"), pend))
        got, dt = wait_for("A sees the rename",
                           lambda: a.entities().get(ub, {}).get("name") == "RenamedByB")
        expect(got, "A sees B's rename (%.1fs): %s" % (dt, a.entities().get(ub, {}).get("name")))

        # ── 9. Deletes, both ways ────────────────────────────────────────────
        ok, sc, _ = b.retry_lock("entity_destroy", {"uuid": ub})
        expect(ok, "B deletes its entity (%s)" % sc.get("code", "ok"))
        got, dt = wait_for("A loses RenamedByB", lambda: ub not in a.entities())
        expect(got, "the delete reached A (%.1fs)" % dt)
        ok, sc, _ = a.retry_lock("entity_destroy", {"uuid": ua})
        expect(ok, "A deletes LiveFromA (%s)" % sc.get("code", "ok"))
        got, dt = wait_for("B loses LiveFromA", lambda: ua not in b.entities())
        expect(got, "the delete reached B (%.1fs)" % dt)
        ents_a, ents_b = a.entities(), b.entities()
        expect(set(ents_a) == set(ents_b), "after all of it both hold the same %d uuids" % len(ents_a))

        # ── 10. B leaves ─────────────────────────────────────────────────────
        left = b.call("collab_leave")
        expect(left.get("inSession") is False, "B is out of the session (%s)" % left.get("status"))
        st, dt = wait_for("A's roster back to 1",
                          lambda: (lambda s: s if len(s.get("participants", [])) == 1 else None)(a.status()))
        expect(st and roster_names(st) == ["Host A"], "A's roster is back to A alone (%.1fs)" % dt)
        a.call("collab_leave")
    except Exception as exc:  # a crash is a failure with its reason, not a traceback
        log("ERROR: %s" % exc)
        failures.append("exception: %s" % exc)
    finally:
        for p in (a, b):
            if p is None:
                continue
            if p.client:
                p.client.close()
            p.ed.stop()
            text = p.ed.log_text()
            lines = [l for l in text.splitlines() if "Collab" in l or "MCP: collab" in l]
            (outdir / ("collab_log_%s.txt" % p.tag.lower())).write_text("\n".join(lines))
            log("%s editor log: %d collab lines" % (p.tag, len(lines)))
            for l in lines[:8]:
                log("    " + l.strip())
        (outdir / "transcript.json").write_text(json.dumps(transcript, indent=1, default=str))

    log("RESULT: %s (%d failures)" % ("PASS" if not failures else "FAIL", len(failures)))
    for f in failures:
        log("  - " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
