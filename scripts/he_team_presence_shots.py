#!/usr/bin/env python3
"""he_team_presence_shots — the team presence footer (topic 202) in real editors.

tests/test_team_ui.cpp draws TeamView's footer in a harness, with a stand-in for
the session's faces (CollabPresenceBar is compiled without ImGui there). This is
the run that harness cannot be: two hidden HorizonEditor processes, Anna (A) and
Bob (B), signed in to a LOCAL team server, with four more members online from
plain sockets — Cleo, Dan, Eli and Fay, each with a colour of their own, Dan
with a picture — so A's footer holds a full stack (4 faces + "+1"). Bob's and
Anna's colours are set the same way, on a second connection of their own.

Pictures come from the editor's HE_DUMP_UI trigger (EditorUiShot.h): the
running editor's whole ImGui frame, rasterised on the CPU. The viewport's scene
image is grey there; everything ImGui draws is real.

Shots (OUTDIR/shots/*.png):
  a-online, b-online      both signed in, no session: the stack, your own face
                          right of the bell;
  a-menu                  the mouse on A's stack: the menu over the docked panels;
  a-hosting, a-menu-hosting  A hosts invitation-only: the stack beside A's
                          session face, Invite enabled;
  a-join-NNN              a burst over Bob's join: his face slides from the
                          stack onto his session face (the log lists, per frame,
                          where Bob's colour is in the footer);
  a-joined, b-joined      after: Bob in A's session, Cleo..Fay still stacked;
  a-left                  Bob leaves the session: back in the stack.

Usage:
    scripts/he_team_presence_shots.py OUTDIR [--editor PATH] [--server-bin PATH]
                                      [--collab-tests DIR] [--timeout S]

Defaults: the build tree's editor (out/build/release), /tmp/collab-build/collab_server
built from ~/VSCode/EngineCollab/CollabSession (profiles need b4a47a5 or later),
whose tests/ supply the wire helpers. Prints `RESULT: PASS|FAIL` last.
"""

import os
import pathlib
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import he_mcp_multiclient as mc  # noqa: E402
from he_collab_two_editors import Peer, wait_for  # noqa: E402

log = mc.log
TIMEOUT = 30.0

# Colours to find in the pictures: none of them is the online dot's green.
COLORS = {"Anna": (60, 130, 240), "Bob": (170, 80, 230), "Cleo": (240, 140, 30),
          "Dan": (200, 200, 200), "Eli": (30, 190, 200), "Fay": (235, 70, 130)}
STACKED = ("Bob", "Cleo", "Dan", "Eli", "Fay")   # online from outside the editors


def team(p):
    return p.call("team_status")


def member(status, name):
    for m in status.get("members", []):
        if m.get("name", "").lower() == name.lower():
            return m
    return None


# ── Pictures ──────────────────────────────────────────────────────────────────

def read_bmp(path):
    """(width, height, rows) of the rasteriser's BMP: 32 bpp, BGRA, top-down."""
    data = pathlib.Path(path).read_bytes()
    off, = struct.unpack_from("<I", data, 10)
    w, h = struct.unpack_from("<ii", data, 18)
    bpp, = struct.unpack_from("<H", data, 28)
    assert bpp == 32, "expected 32 bpp, got %d" % bpp
    top_down = h < 0
    h = abs(h)
    rows = []
    for y in range(h):
        src = y if top_down else h - 1 - y
        rows.append(data[off + src * w * 4: off + (src + 1) * w * 4])
    return w, h, rows


def find_color(path, rgb, band=40, tol=12):
    """Bounding box (x0, y0, x1, y1) of pixels within `tol` of `rgb` in the bottom
    `band` rows (the footer), or None."""
    w, h, rows = read_bmp(path)
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for y in range(max(0, h - band), h):
        row = rows[y]
        for x in range(w):
            b, g, r = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
            if abs(r - rgb[0]) <= tol and abs(g - rgb[1]) <= tol and abs(b - rgb[2]) <= tol:
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    return None if x1 < 0 else (x0, y0, x1, y1)


def to_png(bmp):
    png = bmp.with_suffix(".png")
    r = subprocess.run(["sips", "-s", "format", "png", str(bmp), "--out", str(png)],
                       capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("sips failed on %s: %s" % (bmp, r.stderr))
    return png


class Shots:
    """HE_DUMP_UI for one editor: write the trigger, wait until it is gone."""

    def __init__(self, tag, outdir):
        self.tag = tag
        self.dir = outdir / "shots"
        self.trigger = outdir / ("ui-%s.trigger" % tag)

    def env(self):
        return {"HE_DUMP_UI": str(self.dir), "HE_DUMP_UI_TRIGGER": str(self.trigger)}

    def arm(self, name, frames=1, mouse=None, band=0):
        line = "%s %d" % (name, frames)
        if mouse:
            line += " %d %d" % mouse
        if band:
            line += " band=%d" % band   # the footer only: a full frame costs ~0.7 s
        tmp = self.trigger.with_suffix(".tmp")
        tmp.write_text(line + "\n")
        tmp.rename(self.trigger)   # never a half-written line

    def wait(self, name, frames=1, timeout=60.0):
        deadline = time.time() + timeout
        while self.trigger.exists():
            if time.time() > deadline:
                raise RuntimeError("%s: no UI shot '%s' within %.0fs" % (self.tag, name, timeout))
            time.sleep(0.1)
        return [self.dir / ("%s-%03d.bmp" % (name, i)) for i in range(frames)]

    def shoot(self, name, frames=1, mouse=None):
        self.arm(name, frames, mouse)
        return self.wait(name, frames)


# ── Members online without an editor ─────────────────────────────────────────

class Wire:
    """team_test.py's helpers over one socket per member, kept alive by a pinger
    that also drains every pushed event (a client that does not read its events
    is dropped by the server)."""

    def __init__(self, tests_dir, port):
        sys.dont_write_bytecode = True   # no __pycache__ in the human's checkout
        sys.path.insert(0, str(tests_dir))
        import team_test as tt  # noqa: E402
        self.tt = tt
        self.port = port
        self.socks = {}
        self.lock = threading.Lock()
        self.stop = threading.Event()
        threading.Thread(target=self._pinger, daemon=True).start()

    def login(self, team_name, name, password):
        tt = self.tt
        s = socket.create_connection(("127.0.0.1", self.port), timeout=tt.IO_TIMEOUT)
        s.sendall(tt.team_login(team_name, name, password))
        tt.expect_status(s, tt.LOGGED_IN)
        with self.lock:
            self.socks[name] = s
        return s

    def profile(self, name, rgb, picture=None):
        tt = self.tt
        with self.lock:
            s = self.socks[name]
            s.sendall(tt.set_color(200, rgb))
            tt.expect_reply(s, 200, tt.R_OK)
            if picture:
                s.sendall(tt.set_avatar(201, 64, picture))
                tt.expect_reply(s, 201, tt.R_OK)

    def _pinger(self):
        rid = 1000
        while not self.stop.wait(2.0):
            with self.lock:
                for s in self.socks.values():
                    try:
                        s.sendall(self.tt.ping(rid))
                        s.settimeout(0.05)
                        while s.recv(65536):
                            pass
                    except (socket.timeout, BlockingIOError):
                        pass
                    except OSError:
                        pass
                    finally:
                        try:
                            s.settimeout(self.tt.IO_TIMEOUT)
                        except OSError:
                            pass
                    rid += 1

    def close(self):
        self.stop.set()
        with self.lock:
            for s in self.socks.values():
                s.close()
            self.socks.clear()


def stripes():
    """Dan's 64x64 picture: yellow and navy diagonal stripes, unmistakable."""
    px = bytearray()
    for y in range(64):
        for x in range(64):
            px += bytes((250, 210, 40, 255)) if ((x + y) // 8) % 2 == 0 else bytes((20, 30, 90, 255))
    return bytes(px)


# ── The run ──────────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    editor_path = REPO / "out" / "build" / "release" / "src" / "HE_Editor" / "HorizonEditor"
    server_bin = pathlib.Path("/tmp/collab-build/collab_server")
    tests_dir = pathlib.Path.home() / "VSCode" / "EngineCollab" / "CollabSession" / "tests"
    timeout = 1200
    for i, a in enumerate(args):
        if a == "--editor":
            editor_path = pathlib.Path(args[i + 1])
        if a == "--server-bin":
            server_bin = pathlib.Path(args[i + 1])
        if a == "--collab-tests":
            tests_dir = pathlib.Path(args[i + 1])
        if a == "--timeout":
            timeout = int(args[i + 1])
    for p in (editor_path, server_bin, tests_dir / "team_test.py"):
        if not p.exists():
            log("missing: %s" % p)
            return 2
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)

    sys.dont_write_bytecode = True
    sys.path.insert(0, str(tests_dir))
    from relay_test import Relay  # noqa: E402
    data_dir = outdir / "server-data"
    data_dir.mkdir()
    relay = Relay(str(server_bin), "--data-dir", str(data_dir), "--hash-iterations", "1000")
    server = "127.0.0.1:%d" % relay.port
    log("team server %s on %s" % (server_bin, server))

    os.environ["HE_MCP_COLLAB_CONTROL"] = "1"
    os.environ["HE_COLLAB_OFFLINE"] = "1"
    # Hidden editors are paced to 15 FPS; the 0.4 s slide wants more frames than 6.
    os.environ.setdefault("HE_BACKGROUND_FPS", "30")

    team_name = "presence"
    pw = {n: secrets.token_hex(8) for n in COLORS}
    failures = []
    wire = None
    a = b = None

    def expect(cond, what):
        mc.expect(bool(cond), what, failures)

    try:
        shots_a, shots_b = Shots("a", outdir), Shots("b", outdir)
        os.environ.update(shots_a.env())
        a = Peer("A", outdir / "a", editor_path, timeout)
        os.environ.update(shots_b.env())
        b = Peer("B", outdir / "b", editor_path, timeout)
        t0 = time.time()
        a.connect()
        b.connect()
        log("both endpoints up after %.0fs" % (time.time() - t0))

        # ── A team of six, everyone online, every colour set ─────────────────
        a.call("team_sign_in", {"server": server, "team": team_name, "member": "Anna",
                                "password": pw["Anna"], "create": True})
        st, _ = wait_for("A signed in", lambda: (lambda s: s if s.get("status") in
                                                 ("signed_in", "failed") else None)(team(a)), TIMEOUT)
        if not st or st.get("status") != "signed_in":
            expect(False, "A created the team as Anna: %s" % (st and st.get("error")))
            return 1
        for n in STACKED:
            a.call("team_add_member", {"name": n, "password": pw[n]})
        wait_for("roster of six", lambda: len(team(a).get("members", [])) == 6, TIMEOUT)

        wire = Wire(tests_dir, relay.port)
        for n in COLORS:
            wire.login(team_name, n, pw[n])
            wire.profile(n, COLORS[n], stripes() if n == "Dan" else None)
        b.call("team_sign_in", {"server": server, "team": team_name, "member": "Bob",
                                "password": pw["Bob"]})
        st, _ = wait_for("B signed in", lambda: (lambda s: s if s.get("status") in
                                                 ("signed_in", "failed") else None)(team(b)), TIMEOUT)
        expect(st and st.get("status") == "signed_in", "B signed in as Bob")
        got, dt = wait_for("A sees all online", lambda: all(
            (member(team(a), n) or {}).get("online") for n in COLORS), TIMEOUT)
        expect(got, "A sees all six online (%.1fs)" % dt)
        time.sleep(1.5)   # profile events and Dan's picture download

        # ── 1. No session: the stack, your own face ───────────────────────────
        pa = shots_a.shoot("a-online")[0]
        pb = shots_b.shoot("b-online")[0]
        w, h, _ = read_bmp(pa)
        log("A's frame is %dx%d" % (w, h))
        boxes = {n: find_color(pa, COLORS[n]) for n in COLORS}
        for n, box in boxes.items():
            log("  a-online %-4s %s" % (n, box))
        expect(boxes["Anna"], "A's own face (Anna's colour) is in A's footer")
        expect(boxes["Bob"] and boxes["Cleo"] and boxes["Eli"],
               "Bob, Cleo and Eli are stacked in A's footer")
        expect(not boxes["Fay"], "Fay (5th online) is behind the '+1', not drawn")
        expect(find_color(pa, (250, 210, 40)), "Dan's picture (yellow stripes) is in A's footer")
        if boxes["Bob"] and boxes["Anna"]:
            expect(boxes["Bob"][2] < boxes["Anna"][0], "the stack is left of A's own face")
        if boxes["Bob"] and boxes["Cleo"]:
            expect(boxes["Bob"][0] < boxes["Cleo"][0], "roster order: Bob left of Cleo")
        bb = {n: find_color(pb, COLORS[n]) for n in COLORS}
        expect(bb["Bob"] and bb["Anna"], "B's footer shows Bob (own) and Anna (stacked)")

        # ── 2. Hover: the menu over the docked panels ────────────────────────
        stack = boxes["Cleo"] or boxes["Bob"]
        hover = ((stack[0] + stack[2]) // 2, (stack[1] + stack[3]) // 2) if stack else None
        if hover:
            pm = shots_a.shoot("a-menu", 4, hover)[-1]
            above = find_color(pm, COLORS["Fay"], band=h)   # Fay is only in the menu
            log("  a-menu: Fay's row at %s" % (above,))
            expect(above and above[3] < stack[1], "hovering the stack lists Fay above it")
            shots_a.shoot("a-unhover", 1, (-1, -1))

        # ── 3. A hosts invitation-only: the stack beside the session ─────────
        hs = a.call("team_host", {"open": False})
        sid = hs.get("hostedSession", "")
        wait_for("A hosting", lambda: a.status().get("status") == "hosting", TIMEOUT)
        time.sleep(1.0)
        ph = shots_a.shoot("a-hosting")[0]
        if hover:
            shots_a.shoot("a-menu-hosting", 4, hover)
            shots_a.shoot("a-unhover2", 1, (-1, -1))
        a.call("team_invite", {"member": "Bob"})
        wait_for("B invited", lambda: any(s.get("session") == sid and s.get("invited")
                                          for s in team(b).get("sessions", [])), TIMEOUT)

        # ── 4. Bob joins: the burst ──────────────────────────────────────────
        before = find_color(ph, COLORS["Bob"])
        shots_a.arm("a-join", 45, band=60)
        b.call("team_join", {"session": sid})
        frames = shots_a.wait("a-join", 45, timeout=120)
        joined, _ = wait_for("B joined", lambda: b.status().get("status") == "joined", TIMEOUT)
        expect(joined, "B joined A's session")
        xs = []
        for f in frames:
            box = find_color(f, COLORS["Bob"], tol=40)
            xs.append(box and (box[0] + box[2]) // 2)
            log("  %s Bob %s" % (f.name, box))
        moving = sorted({x for x in xs if x is not None})
        expect(len(moving) >= 3, "Bob's colour shows at %d different x positions over the join"
               % len(moving))
        log("  Bob's stack position before the join: %s" % (before,))

        time.sleep(1.0)
        pj = shots_a.shoot("a-joined")[0]
        shots_b.shoot("b-joined")
        after = {n: find_color(pj, COLORS[n]) for n in COLORS}
        expect(after["Cleo"] and after["Fay"], "after the join Cleo..Fay are stacked (Fay no longer hidden)")
        expect(not after["Bob"], "Bob's team colour has left A's footer (he is a session face now)")

        # ── 5. Bob leaves: back in the stack, no slide back ──────────────────
        b.call("collab_leave")
        wait_for("A alone", lambda: len(a.status().get("participants", [])) == 1, TIMEOUT)
        time.sleep(1.0)
        pl = shots_a.shoot("a-left")[0]
        expect(find_color(pl, COLORS["Bob"]), "Bob is back in A's stack after leaving")
    except Exception as exc:
        log("ERROR: %s" % exc)
        failures.append("exception: %s" % exc)
    finally:
        if wire:
            wire.close()
        for p in (a, b):
            if p is None:
                continue
            if p.client:
                p.client.close()
            text = p.ed.log_text()
            p.ed.stop()
            lines = [l for l in text.splitlines() if "Team" in l or "Collab" in l or "UI shot" in l]
            (outdir / ("team_log_%s.txt" % p.tag.lower())).write_text("\n".join(lines))
        relay.stop()
        pngs = [to_png(bmp) for bmp in sorted((outdir / "shots").glob("*.bmp"))]
        log("%d pictures in %s" % (len(pngs), outdir / "shots"))

    log("RESULT: %s (%d failures)" % ("PASS" if not failures else "FAIL", len(failures)))
    for f in failures:
        log("  - " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
