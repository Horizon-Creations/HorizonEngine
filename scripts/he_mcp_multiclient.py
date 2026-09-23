#!/usr/bin/env python3
"""he_mcp_multiclient — two real MCP clients against one live editor.

Drives the deployed HorizonEditor through its Remote Control bridge with two
(and briefly three) concurrent connections, the way two Claude sessions would:
each connection is authenticated with the token from the endpoint file, exactly
like `he_mcp.py` does (its Bridge class IS the client here), and each asks
`scene_screenshot` for its own camera and its own picture.

What it checks (docs/mcp-scene-screenshot.md, Thema 74, Schritt 4):
  1. the bridge answers from the editor's main loop at all (a paced loop in an
     occluded window can be near-frozen; the probe says so instead of hanging);
  2. two clients get two different, non-zero client ids;
  3. A and B set different cameras → different pictures; B then takes A's
     exact camera → B's picture matches A's (the picture follows the camera,
     not the connection);
  4. A turns; B's stored camera is untouched; A's next picture differs;
  5. A hangs up; a fresh connection has no camera (`stored:false,
     fromViewport:true`) and the editor log shows the goodbye;
  6. optional live-viewport capture with both frustums, then after A is gone
     (`--live`: needs the HE_DUMP_LIVE trigger in the editor).

The editor runs with a private HOME under OUTDIR, so the human's config,
endpoint file and screenshot folder are never touched. Everything (pictures,
JSON replies, editor log) lands in OUTDIR. It also runs in hidden mode
(HE_HIDDEN_WINDOW=1 unless set otherwise, docs/headless-runs.md): the window
is never shown and the focus stays where it was.

Usage:
    scripts/he_mcp_multiclient.py OUTDIR [--probe] [--live] [--editor PATH]
                                         [--port N] [--timeout S]
"""

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
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import he_mcp  # noqa: E402  — the real shim: same handshake, same framing

EDITOR = REPO / "out" / "deploy" / "Editor" / "HorizonEditor"

# The scene: a private copy of the tutorial project (cube at (0,1,0) on a
# 24x24 ground, one point light, sky). Without a project the editor draws no
# debug lines at all — and no gizmos — so a project it is.
PROJECT_SRC = pathlib.Path.home() / "Documents" / "HorizonEngine" / "HorizonTutorial"

# Two cameras on the cube from clearly different sides, both inside the view
# of the editor camera (which starts at (6, 4.5, 6) looking at the origin), so
# the live capture can show both frustums. What matters for the picture test
# is not what is in the picture but that B can later take A's numbers and get
# A's picture.
CAM_A = {"position": [-2.0, 1.5, 3.0], "look_at": [0.0, 1.0, 0.0]}
CAM_B = {"position": [3.0, 1.5, -2.0], "look_at": [0.0, 1.0, 0.0]}
SIZE = {"width": 480, "height": 270}


def log(msg):
    sys.stdout.write("%s %s\n" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


# ── Pictures: BMP/PNG → raw RGB via sips, then a pure-Python pixel diff ──────


def png_to_rgb(png_path):
    """Return (w, h, bytes) using only what macOS ships (sips → BMP)."""
    bmp = str(png_path) + ".bmp"
    subprocess.run(["sips", "-s", "format", "bmp", str(png_path), "--out", bmp],
                   check=True, capture_output=True)
    with open(bmp, "rb") as f:
        data = f.read()
    os.remove(bmp)
    if data[:2] != b"BM":
        raise RuntimeError("sips did not produce a BMP for %s" % png_path)
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp not in (24, 32):
        raise RuntimeError("unexpected BMP depth %d" % bpp)
    stride = ((w * bpp // 8) + 3) & ~3
    rows = []
    step = bpp // 8
    for y in range(abs(h)):
        row = data[off + y * stride: off + y * stride + w * step]
        rows.append(bytes(row[i] for i in range(0, w * step) if i % step < 3))
    if h > 0:
        rows.reverse()
    return w, abs(h), b"".join(rows)


def pixel_diff(png_a, png_b, tol=8):
    """Fraction of pixels whose max channel difference exceeds tol."""
    wa, ha, a = png_to_rgb(png_a)
    wb, hb, b = png_to_rgb(png_b)
    if (wa, ha) != (wb, hb):
        raise RuntimeError("size mismatch %dx%d vs %dx%d" % (wa, ha, wb, hb))
    n = wa * ha
    differ = 0
    for i in range(0, n * 3, 3):
        if (abs(a[i] - b[i]) > tol or abs(a[i + 1] - b[i + 1]) > tol
                or abs(a[i + 2] - b[i + 2]) > tol):
            differ += 1
    return differ / float(n)


# ── The editor process ───────────────────────────────────────────────────────


class Editor:
    def __init__(self, outdir, port, editor_path, timeout):
        self.outdir = pathlib.Path(outdir)
        self.home = self.outdir / "home"
        self.home.mkdir(parents=True, exist_ok=True)
        self.userdata = self.home / "Library" / "Application Support" / "HorizonEngine"
        self.endpoint = self.userdata / "mcp-endpoint.json"
        self.shots = self.userdata / "mcp-screenshots"
        self.logfile = open(self.outdir / "editor.log", "wb")
        # A private copy of the project, opened on start through the private
        # config: the human's project is never written to (ids, autosave).
        self.userdata.mkdir(parents=True, exist_ok=True)
        project = self.outdir / "project"
        shutil.copytree(PROJECT_SRC, project)
        heproj = next(project.glob("*.heproj"))
        with open(self.userdata / "config.json", "w") as f:
            # RHI 4 = Metal, and it has to match HE_DUMP_RHI: the env var only
            # swaps the renderer, EditorUI still picks its ImGui backend from
            # the config (RHI 0 there = ImGui_ImplOpenGL3_NewFrame on a Metal
            # renderer = SIGSEGV in glGetIntegerv on the first UI frame).
            json.dump({"LastProjectPath": str(heproj), "KnownProjects": [str(heproj)],
                       "RHI": 4, "CustomConfig": []}, f)
        env = dict(os.environ)
        env["HOME"] = str(self.home)
        env["HE_MCP"] = "1"
        env["HE_MCP_PORT"] = str(port)
        env.setdefault("HE_DUMP_RHI", "Metal")
        env.setdefault("HE_SKY_TIME", "30")          # no cloud drift between stills
        env.setdefault("HE_COLLAB_OFFLINE", "1")
        # No window, splash, Dock icon or focus grab (docs/headless-runs.md).
        # This run has no frame budget and no HE_DUMP_PATH, so nothing turns
        # hidden mode on by itself; HE_HIDDEN_WINDOW=0 from outside still shows it.
        env.setdefault("HE_HIDDEN_WINDOW", "1")
        self.live_trigger = self.outdir / "live.trigger"
        self.live_bmp = self.outdir / "live.bmp"
        env["HE_DUMP_LIVE"] = str(self.live_bmp)
        env["HE_DUMP_LIVE_TRIGGER"] = str(self.live_trigger)
        self.env = env
        self.timeout = timeout
        self.port = port
        self.proc = subprocess.Popen([str(editor_path)], env=env,
                                     stdout=self.logfile, stderr=subprocess.STDOUT,
                                     cwd=str(editor_path.parent))
        log("editor started, pid %d, HOME=%s" % (self.proc.pid, self.home))

    def wait_endpoint(self):
        deadline = time.time() + self.timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("editor exited early with rc %s" % self.proc.returncode)
            if self.endpoint.exists() and self.endpoint.stat().st_size > 0:
                try:
                    info = he_mcp.read_endpoint(str(self.endpoint))
                    if info["port"] == self.port:
                        log("endpoint file up: port %d pid %d" % (info["port"], info["pid"]))
                        return info
                except he_mcp.ShimError:
                    pass
            time.sleep(0.5)
        raise RuntimeError("no endpoint file after %ds" % self.timeout)

    def live_capture(self, out_png, wait_s=30):
        """Ask the running editor for one capture of its live viewport."""
        if self.live_bmp.exists():
            self.live_bmp.unlink()
        time.sleep(1.0)   # the gizmos trail the tool call by a frame; give it many
        self.live_trigger.write_text("go")
        deadline = time.time() + wait_s
        while time.time() < deadline:
            if not self.live_trigger.exists():      # removed after the write
                if not self.live_bmp.exists():
                    log("  editor consumed the trigger but wrote no picture")
                    return False
                r = subprocess.run(["sips", "-s", "format", "png", str(self.live_bmp),
                                    "--out", str(out_png)], capture_output=True)
                if r.returncode == 0:
                    return True
                raise RuntimeError("sips failed on %s: %s" % (self.live_bmp, r.stderr))
            time.sleep(0.2)
        log("  no live capture within %ds (editor built without the hook?)" % wait_s)
        return False

    def log_text(self):
        self.logfile.flush()
        with open(self.outdir / "editor.log", "rb") as f:
            return f.read().decode("utf-8", "replace")

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(5)
        self.logfile.close()
        log("editor stopped, rc %s" % self.proc.returncode)


# ── Clients ──────────────────────────────────────────────────────────────────


class Client:
    def __init__(self, name, endpoint_path):
        self.name = name
        self.bridge = he_mcp.Bridge(str(endpoint_path))
        self.bridge.ensure()
        self.id = None

    def call(self, tool, args):
        t0 = time.time()
        reply = self.bridge.request("tools/call", {"name": tool, "arguments": args})
        dt = time.time() - t0
        if "error" in reply:
            raise RuntimeError("%s: %s → rpc error %s" % (self.name, tool, reply["error"]))
        res = reply["result"]
        sc = res.get("structuredContent") or {}
        if res.get("isError"):
            raise RuntimeError("%s: %s → tool error %s" % (self.name, tool, res.get("content")))
        return res, sc, dt

    def shot(self, args, outdir, tag):
        """scene_screenshot to file; returns (camera dict, png path in OUTDIR)."""
        a = dict(SIZE)
        a.update(args)
        a["output"] = "file"
        a["name"] = tag
        res, sc, dt = self.call("scene_screenshot", a)
        cam = sc.get("camera") or {}
        if self.id is None:
            self.id = cam.get("client")
        log("  %s [client %s] %s → %.2fs, rendered=%s stored=%s fromViewport=%s "
            "pos=%s yaw=%.1f pitch=%.1f"
            % (self.name, cam.get("client"), tag, dt, sc.get("rendered"),
               cam.get("stored"), cam.get("fromViewport"), cam.get("position"),
               cam.get("yaw", 0.0), cam.get("pitch", 0.0)))
        with open(pathlib.Path(outdir) / (tag + ".json"), "w") as f:
            json.dump(sc, f, indent=1)
        dst = pathlib.Path(outdir) / (tag + ".png")
        shutil.copy(sc["path"], dst)
        return cam, dst

    def close(self):
        self.bridge.close()


# ── The run ──────────────────────────────────────────────────────────────────


def expect(cond, what, failures):
    log(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    probe = "--probe" in args
    live = "--live" in args
    editor_path = EDITOR
    port = 0
    # A Debug editor on macOS 27 needs ~390 s to its endpoint file on an idle
    # machine (no Metal pipeline archive there, everything compiles), and a
    # parallel build on the same box doubles that. 400 was one second short.
    timeout = int(os.environ.get("HE_SHOT_TIMEOUT", "900"))
    for i, a in enumerate(args):
        if a == "--editor":
            editor_path = pathlib.Path(args[i + 1])
        if a == "--port":
            port = int(args[i + 1])
        if a == "--timeout":
            timeout = int(args[i + 1])
    if not port:
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)
    if not editor_path.exists():
        log("no editor at %s" % editor_path)
        return 2

    failures = []
    ed = Editor(outdir, port, editor_path, timeout)
    try:
        ed.wait_endpoint()

        # 1. Does the loop pump? One handshake + tools/list, timed.
        t0 = time.time()
        a = Client("A", ed.endpoint)
        t_auth = time.time() - t0
        t0 = time.time()
        reply = a.bridge.request("tools/list")
        t_list = time.time() - t0
        names = [t["name"] for t in reply.get("result", {}).get("tools", [])]
        log("A: auth %.2fs, tools/list %.2fs, %d tools, scene_screenshot %s"
            % (t_auth, t_list, len(names), "listed" if "scene_screenshot" in names else "MISSING"))
        expect("scene_screenshot" in names, "scene_screenshot is registered", failures)
        expect(t_list < 5.0, "bridge answers within 5s (loop pumps)", failures)
        if probe or failures:
            a.close()
            return 1 if failures else 0

        # 2. Two clients, two cameras, two pictures.
        b = Client("B", ed.endpoint)
        log("phase 2: A and B set different cameras")
        cam_a1, png_a1 = a.shot(CAM_A, outdir, "a1")
        cam_b1, png_b1 = b.shot(CAM_B, outdir, "b1")
        expect(a.id and b.id and a.id != b.id, "two distinct non-zero client ids (%s, %s)" % (a.id, b.id), failures)
        expect(cam_a1.get("stored") and cam_b1.get("stored"), "both cameras stored", failures)
        d_ab = pixel_diff(png_a1, png_b1)
        log("  a1 vs b1: %.1f%% px differ" % (100 * d_ab))
        expect(d_ab > 0.10, "A's and B's pictures differ (>10%%): %.1f%%" % (100 * d_ab), failures)
        if live:
            # Both frustums at their OWN places, before phase 3 puts B onto A:
            # the picture for "correctly positioned" (A left of the cube, B
            # right of it, seen from the editor camera at (6, 4.5, 6)).
            log("live capture with A and B apart")
            expect(ed.live_capture(outdir / "live_apart.png"),
                   "live viewport captured with A and B at their own places", failures)

        # 3. B takes A's camera → B's picture is A's picture.
        log("phase 3: B moves onto A's camera")
        cam_b2, png_b2 = b.shot(CAM_A, outdir, "b2")
        d_b2a1 = pixel_diff(png_b2, png_a1)
        log("  b2 vs a1: %.2f%% px differ" % (100 * d_b2a1))
        expect(d_b2a1 < 0.02, "same camera numbers → same picture (<2%%): %.2f%%" % (100 * d_b2a1), failures)
        expect(cam_b2.get("client") == b.id, "B's reply still carries B's id", failures)
        # A's stored camera did not move when B used A's numbers.
        _, sc, _ = a.call("scene_screenshot", {"render": False})
        expect(sc["camera"]["position"] == cam_a1["position"] and
               abs(sc["camera"]["yaw"] - cam_a1["yaw"]) < 1e-3, "A's camera untouched by B's move", failures)

        # 4. A turns 180°; B stands still; A's next picture differs.
        log("phase 4: A turns, B stays")
        _, sc_a_turn, _ = a.call("scene_screenshot", {"turn": [180.0, 0.0], "render": False})
        _, sc_b_still, _ = b.call("scene_screenshot", {"render": False})
        expect(abs(((sc_a_turn["camera"]["yaw"] - cam_a1["yaw"]) % 360) - 180) < 1e-3,
               "A's yaw moved by 180 (%.1f → %.1f)" % (cam_a1["yaw"], sc_a_turn["camera"]["yaw"]), failures)
        expect(sc_b_still["camera"]["position"] == cam_b2["position"] and
               abs(sc_b_still["camera"]["yaw"] - cam_b2["yaw"]) < 1e-3 and
               abs(sc_b_still["camera"]["pitch"] - cam_b2["pitch"]) < 1e-3,
               "B's camera unchanged by A's turn", failures)
        cam_a2, png_a2 = a.shot({}, outdir, "a2")
        d_a2a1 = pixel_diff(png_a2, png_a1)
        log("  a2 vs a1: %.1f%% px differ" % (100 * d_a2a1))
        expect(d_a2a1 > 0.10, "A's picture changed after the turn (>10%%): %.1f%%" % (100 * d_a2a1), failures)
        # Inline delivery once, through the same connection: image block present.
        res, sc_inl, dt = a.call("scene_screenshot", dict(SIZE))
        kinds = [c.get("type") for c in res.get("content", [])]
        expect("image" in kinds and res["content"][-1].get("mimeType") == "image/png",
               "inline: MCP image block after the text block (%.2fs, %s bytes png)" % (dt, sc_inl.get("pngBytes")), failures)

        # Both frustums live in the viewport (needs the HE_DUMP_LIVE trigger).
        if live:
            log("live capture with both cameras")
            ok = ed.live_capture(outdir / "live_both.png")
            expect(ok, "live viewport captured with A and B connected", failures)
            # Is the capture LIVE? Move the cube through the bridge and look
            # again: a viewport that is being redrawn shows it elsewhere.
            _, sc_list, _ = b.call("entity_list", {})
            cube = next((e for e in sc_list.get("entities", []) if e.get("name") == "Cube"), None)
            if ok and cube:
                b.call("entity_set_transform", {"uuid": cube["uuid"], "position": [0.0, 1.0, -4.0]})
                ok2 = ed.live_capture(outdir / "live_moved.png")
                if ok2:
                    d_mv = pixel_diff(outdir / "live_both.png", outdir / "live_moved.png")
                    log("  live_both vs live_moved (cube moved): %.2f%% px differ" % (100 * d_mv))
                    expect(d_mv > 0.005, "the live capture is live (cube moved: %.2f%%)" % (100 * d_mv), failures)
                b.call("entity_set_transform", {"uuid": cube["uuid"], "position": [0.0, 1.0, 0.0]})
                time.sleep(0.5)

        # 5. A hangs up: cleaned up, B unaffected, newcomer starts empty.
        log("phase 5: A disconnects")
        old_a = a.id
        a.close()
        time.sleep(1.0)
        _, sc_b_after, _ = b.call("scene_screenshot", {"render": False})
        expect(sc_b_after["camera"]["position"] == cam_b2["position"], "B's camera survives A's goodbye", failures)
        c = Client("C", ed.endpoint)
        _, sc_c, _ = c.call("scene_screenshot", dict(SIZE, render=False))
        log("  C [client %s] no-arg: stored=%s fromViewport=%s viewport camera pos=%s yaw=%.1f pitch=%.1f"
            % (sc_c["camera"].get("client"), sc_c["camera"].get("stored"),
               sc_c["camera"].get("fromViewport"), sc_c["camera"].get("position"),
               sc_c["camera"].get("yaw", 0.0), sc_c["camera"].get("pitch", 0.0)))
        expect(sc_c["camera"].get("stored") is False and sc_c["camera"].get("fromViewport") is True,
               "newcomer C has no camera", failures)
        expect(sc_c["camera"].get("client") not in (0, None, old_a),
               "C's id is fresh (%s), not A's old %s" % (sc_c["camera"].get("client"), old_a), failures)
        if live:
            log("live capture after A is gone")
            ok = ed.live_capture(outdir / "live_after.png")
            expect(ok, "live viewport captured after A left", failures)
            if ok and (outdir / "live_both.png").exists():
                # A's frustum is the only thing that changed between the two
                # captures: a few hundred pixels of one colour, no more.
                d_live = pixel_diff(outdir / "live_both.png", outdir / "live_after.png")
                log("  live_both vs live_after: %.2f%% px differ" % (100 * d_live))
                expect(0.0 < d_live < 0.05,
                       "A's gizmo left the viewport, nothing else moved: %.2f%%" % (100 * d_live),
                       failures)
        c.close()
        b.close()
        time.sleep(0.5)
        text = ed.log_text()
        gone_lines = [l for l in text.splitlines() if "MCP:" in l and ("gone" in l or "disconnect" in l.lower() or "left" in l)]
        log("  editor log MCP lines about leaving: %d" % len(gone_lines))
        for l in gone_lines[:6]:
            log("    " + l.strip())
        with open(outdir / "mcp_log_lines.txt", "w") as f:
            f.write("\n".join(l for l in text.splitlines() if "MCP" in l))
    finally:
        ed.stop()
        if ed.shots.exists():
            log("tool stills in private folder: %d" % len(list(ed.shots.iterdir())))

    log("RESULT: %s (%d failures)" % ("PASS" if not failures else "FAIL", len(failures)))
    for f in failures:
        log("  - " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
