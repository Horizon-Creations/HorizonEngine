#!/usr/bin/env python3
"""he_team_sync_acceptance — topic 199 step 5: design section 10 with two real editors.

docs/team-server-source-control-design-2026-10-13.md section 10 lists what "done" means
for the team server as source-control mediator. scripts/he_team_sync_two_editors.py is
the first half of 1 on loopback; this runs the cases through two deployed editors
(Anna, admin; Bob) against a local collab_server or the rented host, every change made
through the editors' own tools (MCP), every save the editor's own Save:

  1. both editors add an entity to the same scene, both save: both in head, `merged`;
  2. both add a child to the same parent: both children, under that parent, both sides;
  3. both move the same entity: the later save's value on both sides, the earlier
     one named on `horizon-conflicts` (`merged-arrival … lost=horizon-conflicts:…`);
  4. both change the same material (.hasset, whole file): one file on both disks,
     `replaced … lost=…`;
  5. Anna deletes a material Bob changes: the material is still there on both disks
     (`delete-dropped` or `restored`, depending on which save the server took first).

What the server did is read the way any member's client can, through a sync
connection of the script's own (HEAD, LOG with the Horizon-Merge trailers, GET of the
overruled version), so the run needs no server log and works against a remote host.

Backup (6, 7) with --backup-url: BACKUP_SET as Anna, the deploy key registered on
GitHub with `gh` when --github-repo OWNER/REPO is given (the repo must exist; nothing is
created or deleted here), then BACKUP_STATUS until everything up to head is pushed.
--lfs adds a 60 MiB .png (LFS on GitHub) and a 120 MiB non-media file (refused).

Usage:
    scripts/he_team_sync_acceptance.py OUTDIR [--server HOST:PORT] [--server-bin PATH]
        [--editor PATH] [--backup-url URL --github-repo OWNER/REPO [--lfs]]

Without --server it starts collab_server on loopback with its own data folder. Against
a remote server the team it creates (he-e2e-199-NNNNNN) stays there: the protocol has
no way to delete a team. Prints `RESULT: PASS|FAIL` last.
"""

import argparse
import hashlib
import json
import os
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
import he_team_sync_two_editors as two  # noqa: E402
from he_collab_two_editors import Peer, wait_for, free_port  # noqa: E402
from he_team_sync_two_editors import frame, str8, recv_frame, team_request  # noqa: E402

log = mc.log
SYNC_TIMEOUT = 60.0
SCENE = ".hescene"

# Sync ops (CollabSession src/Protocol.h ScOp) and backup states.
HEAD, GET, LOG, BACKUP_SET, BACKUP_STATUS = 1, 4, 7, 9, 10
BACKUP_STATES = {1: "pending", 2: "ok", 3: "failing", 4: "diverged"}


# ── The script's own member client: HEAD, LOG, GET, BACKUP_* ─────────────────
class Observer:
    """One fresh login + ticket + sync connection per request: nothing to keep alive,
    and a remote server is read exactly like the local one."""

    def __init__(self, host, port, team, project, member, password):
        self.addr = (host, port)
        self.team, self.project, self.member, self.password = team, project, member, password

    def _open(self):
        s = socket.create_connection(self.addr, timeout=20)
        try:
            s.sendall(frame(6, bytes([1]) + str8(self.team) + str8(self.member) + str8(self.password)))
            ftype, body = recv_frame(s)
            if ftype != 2 or body[0] != 4:
                raise RuntimeError("observer login refused: %r" % (body,))
            result, tk = team_request(s, 1, 20, str8(self.project))
            if result != 0:
                raise RuntimeError("observer ticket refused: %d" % result)
            c = socket.create_connection(self.addr, timeout=20)
            c.sendall(frame(10, str8(self.team) + str8(self.member) + str8(self.project) + tk))
            ftype, body = recv_frame(c)
            if ftype != 2 or body[0] != 5:
                c.close()
                raise RuntimeError("observer sync hello refused: %r" % (body,))
            return c
        finally:
            s.close()

    def request(self, op, fields=b""):
        """(result, text, body, stream)"""
        c = self._open()
        try:
            c.sendall(frame(11, struct.pack(">I", 1) + bytes([op]) + fields))
            stream = b""
            while True:
                ftype, body = recv_frame(c)
                if ftype == 13:                       # SC_DATA: id u32, offset u64, last u8
                    stream += body[13:]
                elif ftype == 12:                     # SC_REPLY: id u32, result u8, text str16
                    _, result, n = struct.unpack(">IBH", body[:7])
                    return result, body[7:7 + n].decode(errors="replace"), body[7 + n:], stream
        finally:
            c.close()

    def head(self):
        result, text, body, _ = self.request(HEAD)
        if result != 0:
            raise RuntimeError("HEAD: %d %s" % (result, text))
        return struct.unpack(">Q", body[:8])[0]

    def log(self, from_rev=1, count=1000):
        """[{rev, member, subject, merges: [trailer value, ...]}] oldest first."""
        result, text, _, stream = self.request(LOG, struct.pack(">QH", from_rev, count))
        if result != 0:
            raise RuntimeError("LOG: %d %s" % (result, text))
        revs = []
        for line in stream.decode(errors="replace").splitlines():
            parts = line.split("\t")
            if parts[0] == "R" and len(parts) >= 5:
                revs.append({"rev": int(parts[1]), "member": parts[2], "subject": parts[4], "merges": []})
            elif parts[0] == "M" and revs:
                revs[-1]["merges"].append("\t".join(parts[1:]))
        return revs

    def get(self, digest_hex):
        result, text, _, stream = self.request(GET, bytes.fromhex(digest_hex))
        if result != 0:
            raise RuntimeError("GET: %d %s" % (result, text))
        return stream

    def backup_set(self, url, branch=""):
        u = url.encode()
        result, text, body, _ = self.request(BACKUP_SET, struct.pack(">H", len(u)) + u + str8(branch))
        if result != 0:
            raise RuntimeError("BACKUP_SET: %d %s" % (result, text))
        (n,) = struct.unpack(">H", body[:2])
        return body[2:2 + n].decode()

    def backup_status(self):
        result, text, body, _ = self.request(BACKUP_STATUS)
        if result != 0:
            return {"result": result, "text": text}
        pushed, state = struct.unpack(">QB", body[:9])
        (n,) = struct.unpack(">H", body[9:11])
        err = body[11:11 + n].decode(errors="replace")
        (t,) = struct.unpack(">Q", body[11 + n:19 + n])
        return {"pushed_rev": pushed, "state": BACKUP_STATES.get(state, state), "error": err, "time": t}


def sha_file(p):
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--server", help="HOST:PORT of a running team server (default: start one on loopback)")
    ap.add_argument("--server-bin", default="/tmp/collab-build/collab_server")
    ap.add_argument("--editor", default=str(mc.EDITOR))
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--backup-url", help="BACKUP_SET target (git@github.com:O/R.git, or a local path with a local server)")
    ap.add_argument("--github-repo", help="OWNER/REPO: register the returned deploy key there with gh")
    ap.add_argument("--lfs", action="store_true", help="also save a 60 MiB .png and a 120 MiB non-media file")
    args = ap.parse_args()
    out = pathlib.Path(args.outdir)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    failures, notes = [], []

    def expect(cond, what):
        log(("  ok    " if cond else "  FAIL  ") + what)
        if not cond:
            failures.append(what)
        return cond

    team = "he-e2e-199-%d" % (int(time.time()) % 1000000)
    two.TEAM = team
    project = two.PROJECT
    server = server_log = None
    if args.server:
        host, port = args.server.rsplit(":", 1)
        port = int(port)
    else:
        host, port = "127.0.0.1", free_port()
        server_log = open(out / "server.log", "wb")
        extra = ["--sc-backup-allow-local", "--sc-backup-delay", "2", "--sc-backup-max-delay", "10"] \
            if args.backup_url else []
        server = subprocess.Popen([args.server_bin, str(port), "--data-dir", str(out / "server-data")] + extra,
                                  stdout=server_log, stderr=subprocess.STDOUT)
        time.sleep(0.5)
    address = "%s:%d" % (host, port)
    log("team server %s, team %s, project %s" % (address, team, project))
    obs = Observer(host, port, team, project, "Anna", "anna-password")
    a = b = None
    anna_root = out / "anna" / "project"
    bob_root = out / "bob" / "project"

    def wait_head(n, what):
        v, t = wait_for(what, lambda: obs.head() >= n, timeout=SYNC_TIMEOUT, interval=0.5)
        expect(bool(v), "%s: head reached r%d (%.1fs)" % (what, n, t))
        return bool(v)

    def revs_since(h0):
        return [r for r in obs.log(h0 + 1) if r["rev"] > h0]

    def merges_matching(revs, needle, outcomes):
        hits = []
        for r in revs:
            for m in r["merges"]:
                if needle in m and m.split(" ", 1)[0] in outcomes:
                    hits.append((r["rev"], r["member"], m))
        return hits

    def state_rev(root):
        st = root / "Saved" / "TeamSync" / ("%s-%s.state" % (team.lower(), project))
        if not st.exists():
            return -1
        for line in st.read_text().splitlines():
            if line.startswith("rev\t"):
                return int(line.split("\t")[1])
        return -1

    def both_at_head(what):
        h = obs.head()
        v, t = wait_for(what, lambda: state_rev(anna_root) >= h and state_rev(bob_root) >= h,
                        timeout=SYNC_TIMEOUT, interval=0.5)
        expect(bool(v), "%s: both editors' sync state at r%d (%.1fs)" % (what, h, t))

    def entity(peer, name):
        hits = peer.by_name(name)
        return hits[0] if len(hits) == 1 else None

    def world_x(peer, name):
        e = entity(peer, name)
        if not e:
            return None
        return peer.call("entity_get", {"uuid": e["uuid"]}).get("worldPosition", [None])[0]

    def first_param(peer, path):
        """A parameter material_set_param can set to a scalar: (name, value(x)) or None."""
        info = peer.call("material_info", {"path": path})
        for p in info.get("params", []):
            if p.get("kind") == "float":
                return p["name"], lambda x: x
            if p.get("kind") == "color":
                return p["name"], lambda x: [x, x, x]
        notes.append("material_info %s: %s" % (path, json.dumps(info)[:600]))
        return None

    try:
        two.set_up_server(port, host)
        template = out / "template-anna"
        two.team_mode_template(template, mc.PROJECT_SRC)
        os.environ["HE_MCP_COLLAB_CONTROL"] = "1"

        # ── Setup: Anna r1 with a shared parent, Bob from a copy of that tree ───
        mc.PROJECT_SRC = template
        a = Peer("A", out / "anna", pathlib.Path(args.editor), args.timeout)
        a.connect()
        a.call("team_sign_in", {"server": address, "team": team, "member": "Anna", "password": "anna-password"})
        a.call("entity_create", {"name": "E2E_Parent", "position": [0.0, 1.0, 0.0]})
        # The sign-in is asynchronous: save until the first revision is there.
        v, t = wait_for("Anna's first revision", lambda: obs.head() >= 1 or (a.call("scene_save") and False),
                        timeout=SYNC_TIMEOUT, interval=3.0)
        expect(bool(v), "setup: Anna's save went to the team server as r1 (%.1fs)" % t)
        a.call("material_create", {"path": "Materials/E2E_Shared.hasset"})
        a.call("material_create", {"path": "Materials/E2E_Doomed.hasset"})
        a.call("scene_save")
        v, t = wait_for("Anna's materials", lambda: any("E2E_Doomed" in m for r in obs.log() for m in r["merges"]),
                        timeout=SYNC_TIMEOUT, interval=0.5)
        expect(bool(v), "setup: Anna's two new materials are on the server (%.1fs)" % t)

        bob_src = out / "template-bob"
        shutil.copytree(anna_root, bob_src, ignore=shutil.ignore_patterns("Saved"))
        mc.PROJECT_SRC = bob_src
        b = Peer("B", out / "bob", pathlib.Path(args.editor), args.timeout)
        b.connect()
        b.call("team_sign_in", {"server": address, "team": team, "member": "Bob", "password": "bob-password"})
        both_at_head("setup")
        parent = entity(b, "E2E_Parent")
        expect(parent is not None, "setup: Bob's editor has the shared parent")
        parent_uuid = parent["uuid"] if parent else ""

        # ── 1. Different entities, same scene ──────────────────────────────────
        h0 = obs.head()
        a.call("entity_create", {"name": "E2E_Anna_1", "position": [2.0, 0.5, 0.0]})
        b.call("entity_create", {"name": "E2E_Bob_1", "position": [-2.0, 0.5, 0.0]})
        a.call("scene_save")
        b.call("scene_save")
        wait_head(h0 + 2, "1 different entities")
        both_at_head("1")
        for peer, who in ((a, "Anna"), (b, "Bob")):
            v, t = wait_for("1 on " + who, lambda p=peer: entity(p, "E2E_Anna_1") and entity(p, "E2E_Bob_1"),
                            timeout=SYNC_TIMEOUT)
            expect(bool(v), "1: %s's editor shows both new entities, once each (%.1fs)" % (who, t))
        hits = merges_matching(revs_since(h0), SCENE, ("merged",))
        expect(bool(hits), "1: the server merged the scene: %s" % (hits or revs_since(h0)))

        # ── 2. Both add a child to the same parent ─────────────────────────────
        h0 = obs.head()
        a.call("entity_create", {"name": "E2E_ChildAnna", "parent": parent_uuid, "position": [1.0, 0.0, 0.0]})
        b.call("entity_create", {"name": "E2E_ChildBob", "parent": parent_uuid, "position": [-1.0, 0.0, 0.0]})
        a.call("scene_save")
        b.call("scene_save")
        wait_head(h0 + 2, "2 children of one parent")
        both_at_head("2")
        for peer, who in ((a, "Anna"), (b, "Bob")):
            def kids(p=peer):
                ca, cb = entity(p, "E2E_ChildAnna"), entity(p, "E2E_ChildBob")
                return ca and cb and ca.get("parent") == parent_uuid and cb.get("parent") == parent_uuid
            v, t = wait_for("2 on " + who, kids, timeout=SYNC_TIMEOUT)
            expect(bool(v), "2: %s's editor has both children under the shared parent (%.1fs)" % (who, t))
        hits = merges_matching(revs_since(h0), SCENE, ("merged",))
        expect(bool(hits), "2: the server merged the scene: %s" % (hits or revs_since(h0)))

        # ── 3. Same field: the later save wins, the earlier one is kept aside ──
        h0 = obs.head()
        a.call("entity_set_transform", {"uuid": parent_uuid, "position": [5.0, 1.0, 0.0]})
        b.call("entity_set_transform", {"uuid": parent_uuid, "position": [7.0, 1.0, 0.0]})
        a.call("scene_save")
        wait_head(h0 + 1, "3 Anna's move")
        b.call("scene_save")
        wait_head(h0 + 2, "3 Bob's move")
        both_at_head("3")
        for peer, who in ((a, "Anna"), (b, "Bob")):
            v, t = wait_for("3 on " + who, lambda p=peer: (world_x(p, "E2E_Parent") or 0) > 6.5, timeout=SYNC_TIMEOUT)
            expect(bool(v), "3: %s's editor shows the later save's position x=7 (%.1fs)" % (who, t))
        revs = revs_since(h0)
        hits = merges_matching(revs, SCENE, ("merged-arrival",))
        expect(bool(hits) and hits[-1][1] == "Bob" and "lost=horizon-conflicts:" in hits[-1][2],
               "3: Bob's later save is `merged-arrival` and names Anna's version on horizon-conflicts: %s"
               % (hits or revs))

        # ── 4. Same material (whole file): one wins, the other is kept aside ───
        param = first_param(a, "Materials/E2E_Shared.hasset")
        expect(param is not None, "4: the new material has a parameter to set (%s)" % (param and param[0]))
        if param:
            name, value = param
            h0 = obs.head()
            a.call("material_set_param", {"path": "Materials/E2E_Shared.hasset", "name": name, "value": value(0.25)})
            # Not 0.8: that is the template's BaseColor, and an unchanged file is no save.
            b.call("material_set_param", {"path": "Materials/E2E_Shared.hasset", "name": name, "value": value(0.55)})
            wait_head(h0 + 2, "4 same material")
            both_at_head("4")
            rel = pathlib.Path("Content/Materials/E2E_Shared.hasset")
            v, t = wait_for("4 same file", lambda: sha_file(anna_root / rel) == sha_file(bob_root / rel),
                            timeout=SYNC_TIMEOUT)
            expect(bool(v), "4: Anna's and Bob's disks hold the same E2E_Shared.hasset (%.1fs)" % t)
            revs = revs_since(h0)
            hits = merges_matching(revs, "E2E_Shared", ("replaced",))
            expect(bool(hits) and "lost=horizon-conflicts:" in hits[-1][2],
                   "4: the later save `replaced` the material and kept the other on horizon-conflicts: %s"
                   % (hits or revs))

        # ── 5. Delete against change: the material survives ────────────────────
        param = first_param(b, "Materials/E2E_Doomed.hasset")
        if param:
            name, value = param
            h0 = obs.head()
            b.call("material_set_param", {"path": "Materials/E2E_Doomed.hasset", "name": name, "value": value(0.6)})
            a.call("asset_delete", {"path": "Materials/E2E_Doomed.hasset", "force": True})
            a.call("scene_save")    # the delete itself is no asset save: this is Anna's sync trigger
            wait_head(h0 + 2, "5 delete against change")
            both_at_head("5")
            rel = pathlib.Path("Content/Materials/E2E_Doomed.hasset")
            v, t = wait_for("5 still there", lambda: (anna_root / rel).exists() and
                            sha_file(anna_root / rel) == sha_file(bob_root / rel), timeout=SYNC_TIMEOUT)
            expect(bool(v), "5: E2E_Doomed.hasset is on both disks again, with Bob's change (%.1fs)" % t)
            revs = revs_since(h0)
            hits = merges_matching(revs, "E2E_Doomed", ("delete-dropped", "restored"))
            expect(bool(hits), "5: the server kept the material (%s)" % (hits or revs))

        # ── 6/7. Backup ─────────────────────────────────────────────────────────
        if args.backup_url:
            pub = obs.backup_set(args.backup_url)
            (out / "deploy-key.pub").write_text(pub + "\n")
            expect(pub.startswith("ssh-ed25519 "), "6: BACKUP_SET as Anna returned a deploy key")
            if args.github_repo:
                r = subprocess.run(["gh", "api", "-X", "POST", "repos/%s/keys" % args.github_repo,
                                    "-f", "title=horizon-team-server " + team, "-f", "key=" + pub,
                                    "-F", "read_only=false"], capture_output=True, text=True)
                expect(r.returncode == 0, "6: deploy key registered on %s (%s)" % (args.github_repo,
                                                                                   r.stderr.strip()[:200]))
            if args.lfs:
                big = anna_root / "Content" / "E2E_Big.png"
                big.write_bytes(b"\x89PNG\r\n\x1a\n" + os.urandom(60 * 1024 * 1024))
                h0 = obs.head()
                a.call("scene_save")
                wait_head(h0 + 1, "7 a 60 MiB texture")
                hits = merges_matching(revs_since(h0), ".gitattributes", ("merged",))
                expect(bool(hits), "7: the server put the texture under LFS: %s" % hits)
            h = obs.head()
            v, t = wait_for("backup", lambda: (lambda s: s.get("state") == "ok" and s.get("pushed_rev", 0) >= h)(
                obs.backup_status()), timeout=420, interval=3.0)
            st = obs.backup_status()
            (out / "backup-status.json").write_text(json.dumps(st, indent=2))
            expect(bool(v), "6: backup state ok with everything up to r%d pushed (%.0fs): %s" % (h, t, st))
            if args.lfs:
                huge = bob_root / "Content" / "E2E_Huge.bin"
                huge.write_bytes(os.urandom(120 * 1024 * 1024))
                h0 = obs.head()
                b.call("scene_save")
                time.sleep(20)
                expect(obs.head() == h0, "7: a 120 MiB non-media file made no revision (head stays r%d)" % h0)
                huge.unlink()
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
        try:
            hist = obs.log()
            (out / "server-history.json").write_text(json.dumps(hist, indent=2))
            log("server history:\n" + "\n".join("r%d %s: %s%s" % (r["rev"], r["member"], r["subject"],
                                                 "".join("\n    " + m for m in r["merges"])) for r in hist))
        except Exception as e:  # noqa: BLE001
            log("server history unreadable: %s" % e)
        if server:
            server.terminate()
            try:
                server.wait(40)
            except subprocess.TimeoutExpired:
                server.kill()
            server_log.close()

    for n in notes:
        log("note: " + n)
    log("RESULT: %s" % ("PASS" if not failures else "FAIL (%d)" % len(failures)))
    for f in failures:
        log("  - " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
