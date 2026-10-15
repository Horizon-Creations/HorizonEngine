#!/usr/bin/env python3
"""he_team_sync_two_editors — two real editors save through one team server (topic 199).

tests/test_team_sync*.cpp drive TeamSyncService and TeamSyncController inside the
test binary. This is the run they cannot be: two deployed HorizonEditor processes
whose project says sourceControl.mode = "teamServer", signed in to a real
collab_server (started here, on loopback, with its own data folder) as two members
of one team. The saves come from the editors' own Save (the MCP scene_save tool),
so the editor's hooks are what is tested: save -> debounced upload -> server merge
-> sc-head on the other editor's team connection -> download -> scene reload.

  1. The script creates team, member Bob and project "game" on the server with a
     few lines of protocol (no MCP tool reaches the Create Project button).
  2. Anna's editor opens the project, signs in, saves the scene: r1 (the whole tree).
  3. Bob's editor opens a copy of Anna's tree at r1 (without Saved/) and signs in:
     its first sync finds every file equal to the team's.
  4. Anna adds an entity and saves: r2. Bob's editor gets it without saving
     anything and shows it (the scene reloads; it was clean).
  5. Bob adds another entity and saves: r3, merged by the server with Anna's r2 by
     entity. Anna's editor shows both.

Usage:
    scripts/he_team_sync_two_editors.py OUTDIR [--server-bin PATH] [--editor PATH] [--timeout S]

Prints `RESULT: PASS|FAIL` last; editor logs, the server log and a transcript go into OUTDIR.
"""

import argparse
import json
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import he_mcp_multiclient as mc  # noqa: E402
from he_collab_two_editors import Peer, wait_for, free_port  # noqa: E402

log = mc.log
TEAM, PROJECT = "sync-e2e-%d" % (int(time.time()) % 1000000), "game"
SYNC_TIMEOUT = 40.0


# ── A few lines of the team protocol, to set the server up ───────────────────
def frame(ftype, body):
    payload = b"HERL" + bytes([1, ftype]) + body
    return struct.pack(">I", len(payload)) + payload


def str8(s):
    b = s.encode()
    return bytes([len(b)]) + b


def recv_frame(sock):
    def exact(n):
        out = b""
        while len(out) < n:
            chunk = sock.recv(n - len(out))
            if not chunk:
                raise RuntimeError("server closed the connection")
            out += chunk
        return out
    (n,) = struct.unpack(">I", exact(4))
    payload = exact(n)
    if payload[:5] != b"HERL\x01":
        raise RuntimeError("not a HERL frame")
    return payload[5], payload[6:]


def team_request(sock, rid, op, fields=b""):
    sock.sendall(frame(7, struct.pack(">I", rid) + bytes([op]) + fields))
    while True:
        ftype, body = recv_frame(sock)
        if ftype == 9:       # an event between the replies
            continue
        if ftype != 8:
            raise RuntimeError("unexpected frame %d" % ftype)
        (got,) = struct.unpack(">I", body[:4])
        if got != rid:
            continue
        result = body[4]
        tlen = body[5]
        return result, body[6 + tlen:]


def set_up_server(port, host="127.0.0.1"):
    """Team TEAM with admin Anna and member Bob, project PROJECT (empty)."""
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(frame(6, bytes([2]) + str8(TEAM) + str8("Anna") + str8("anna-password")))
    ftype, body = recv_frame(s)
    if ftype != 2 or body[0] != 4:
        raise RuntimeError("create team refused: %r" % (body,))
    result, _ = team_request(s, 1, 2, str8("Bob") + str8("bob-password") + bytes([0]))
    if result != 0:
        raise RuntimeError("add member refused: %d" % result)
    result, ticket = team_request(s, 2, 20, str8(PROJECT))
    if result != 0 or len(ticket) != 16:
        raise RuntimeError("sc ticket refused: %d" % result)
    c = socket.create_connection((host, port), timeout=10)
    c.sendall(frame(10, str8(TEAM) + str8("Anna") + str8(PROJECT) + ticket))
    ftype, body = recv_frame(c)
    if ftype != 2 or body[0] != 5:
        raise RuntimeError("sync hello refused: %r" % (body,))
    c.sendall(frame(11, struct.pack(">I", 1) + bytes([8])))     # CREATE_PROJECT
    ftype, body = recv_frame(c)
    if ftype != 12 or body[4] != 0:
        raise RuntimeError("create project refused: %r" % (body,))
    c.close()
    s.close()
    log("server: team %s, members Anna (admin) and Bob, project %s created" % (TEAM, PROJECT))


def team_mode_template(dst, src):
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns("Saved"))
    cfg = dst / "Config"
    cfg.mkdir(exist_ok=True)
    (cfg / "ProjectSettings.json").write_text(json.dumps(
        {"version": 1, "sourceControl": {"mode": "teamServer", "team": TEAM, "project": PROJECT}},
        indent=4) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--server-bin", default="/tmp/collab-build/collab_server")
    ap.add_argument("--editor", default=str(mc.EDITOR))
    ap.add_argument("--timeout", type=float, default=600.0)
    args = ap.parse_args()
    out = pathlib.Path(args.outdir)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    failures = []

    def expect(cond, what):
        log(("  ok    " if cond else "  FAIL  ") + what)
        if not cond:
            failures.append(what)
        return cond

    port = free_port()
    server_log = open(out / "server.log", "wb")
    server = subprocess.Popen([args.server_bin, str(port), "--data-dir", str(out / "server-data")],
                              stdout=server_log, stderr=subprocess.STDOUT)
    address = "127.0.0.1:%d" % port
    a = b = None

    def server_text():
        server_log.flush()
        return (out / "server.log").read_text(errors="replace")

    def saved_rev(member, rev):
        return ("%s saved rev %d of" % (member, rev)) in server_text()

    try:
        time.sleep(0.5)
        set_up_server(port)
        template = out / "template-anna"
        team_mode_template(template, mc.PROJECT_SRC)

        # ── 2. Anna: open, sign in, save: r1 ───────────────────────────────────
        import os
        os.environ["HE_MCP_COLLAB_CONTROL"] = "1"
        mc.PROJECT_SRC = template
        a = Peer("A", out / "anna", pathlib.Path(args.editor), args.timeout)
        a.connect()
        a.call("team_sign_in", {"server": address, "team": TEAM, "member": "Anna",
                                "password": "anna-password"})
        _, t = wait_for("Anna's sync connection",
                        lambda: "member=Anna project=%s] opened a sync connection" % PROJECT in server_text(),
                        timeout=SYNC_TIMEOUT)
        expect(t < SYNC_TIMEOUT, "Anna's editor opened a sync connection after signing in")
        a.call("scene_save")
        v, t = wait_for("r1", lambda: saved_rev("Anna", 1), timeout=SYNC_TIMEOUT)
        expect(bool(v), "Anna's Save went to the team server as r1 (%.1fs)" % t)

        # ── 3. Bob: a copy of Anna's tree at r1 ────────────────────────────────
        bob_src = out / "template-bob"
        shutil.copytree(out / "anna" / "project", bob_src, ignore=shutil.ignore_patterns("Saved"))
        mc.PROJECT_SRC = bob_src
        b = Peer("B", out / "bob", pathlib.Path(args.editor), args.timeout)
        b.connect()
        b.call("team_sign_in", {"server": address, "team": TEAM, "member": "Bob",
                                "password": "bob-password"})
        # The witness is the state file the first sync writes (the editor's own
        # log is block-buffered while it runs): r1, and a base for each of the
        # files of r1, which only happens when every one of them was equal.
        state = out / "bob" / "project" / "Saved" / "TeamSync" / ("%s-%s.state" % (TEAM.lower(), PROJECT))

        def bob_bases():
            if not state.exists():
                return None
            lines = state.read_text().splitlines()
            if "rev\t1" not in lines:
                return None
            return [x for x in lines if x.startswith("B\t")]
        v, t = wait_for("Bob's first sync", bob_bases, timeout=SYNC_TIMEOUT)
        expect(bool(v) and len(v) == 6,
               "Bob's first sync found his copy equal to the team's r1 (%d bases, %.1fs)" % (len(v or []), t))

        # ── 4. Anna adds an entity and saves: Bob shows it without doing anything ──
        a.call("entity_create", {"name": "TeamSyncMarkerAnna", "position": [2.0, 0.5, 0.0]})
        a.call("scene_save")
        v, t = wait_for("r2", lambda: saved_rev("Anna", 2), timeout=SYNC_TIMEOUT)
        expect(bool(v), "Anna's second Save went up as r2 (%.1fs)" % t)
        v, t = wait_for("Bob sees Anna's marker", lambda: b.by_name("TeamSyncMarkerAnna"),
                        timeout=SYNC_TIMEOUT)
        expect(bool(v), "Bob's editor shows Anna's new entity, without a save of his (%.1fs)" % t)

        # ── 5. Bob adds his own and saves: the server merges by entity ──────────
        b.call("entity_create", {"name": "TeamSyncMarkerBob", "position": [-2.0, 0.5, 0.0]})
        b.call("scene_save")
        v, t = wait_for("r3", lambda: saved_rev("Bob", 3), timeout=SYNC_TIMEOUT)
        expect(bool(v), "Bob's Save went up as r3 (%.1fs)" % t)
        v, t = wait_for("Anna sees both", lambda: a.by_name("TeamSyncMarkerBob") and
                        a.by_name("TeamSyncMarkerAnna"), timeout=SYNC_TIMEOUT)
        expect(bool(v), "Anna's editor shows Bob's entity next to her own (%.1fs)" % t)
        expect(len(b.by_name("TeamSyncMarkerAnna")) == 1 and len(b.by_name("TeamSyncMarkerBob")) == 1,
               "Bob's editor still has both entities, once each")
    except Exception as e:  # noqa: BLE001
        failures.append("exception: %s" % e)
        log("EXCEPTION: %s" % e)
    finally:
        for p in (a, b):
            if p:
                try:
                    if p.client:
                        p.client.close()
                finally:
                    p.ed.stop()
        server.terminate()
        try:
            server.wait(10)
        except subprocess.TimeoutExpired:
            server.kill()
        server_log.close()
        repo = next((out / "server-data" / "sc").glob("*/%s/repo.git" % PROJECT), None)
        if repo:
            r = subprocess.run(["git", "-C", str(repo), "log", "--format=%an: %s%n%(trailers:key=Horizon-Merge,valueonly)",
                                "main"], capture_output=True, text=True)
            (out / "server-history.txt").write_text(r.stdout)
            log("server history:\n" + r.stdout.strip())

    log("RESULT: %s" % ("PASS" if not failures else "FAIL (%d)" % len(failures)))
    for f in failures:
        log("  - " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
