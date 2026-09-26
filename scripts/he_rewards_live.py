#!/usr/bin/env python3
"""he_rewards_live — the reward counters (Thema 75) against a live editor.

Drives the deployed HorizonEditor through its Remote Control bridge and checks
what can be checked WITHOUT a screen and a keyboard: the BuildSucceeded moment
and the persistent progress counters behind the footer's
"Ready · N builds today · N days in a row" (EditorRewards.h).

The build is an export (`project_package`, target Host): it reports into
BuildProgressDialog exactly like Build > Export Project from the menu, and
EditorUI's per-frame edge detector over BuildProgressDialog::outcome() is the
one hook both kinds of build go through. The counters are read straight from
the private config.json the editor writes them into.

  Run 1  fresh config:   nothing counted before any moment;
                         export → RewardsBuildsToday 1, RewardsDay today,
                         RewardsStreakDays 1; second export → 2 (once per run,
                         not once per poll); a failing export → still 2;
                         rewards.enabled off → export → still 2;
                         back on → still 2 (the run from the off phase is not
                         rewarded late) → export → 3.
  Run 2  restart:        the counters survived (3 in the file before start);
                         export → 4 (continued from the file, not reset).
  Run 3  day rollover:   RewardsDay set to yesterday by hand before start;
                         export → buildsToday 1, streak 2.

NOT checked here, and not checkable this way: Saved (scene_save bypasses the
reward on purpose — an agent's save is not the user's), Assets imported (no
MCP tool imports), the footer line itself and the chime (no picture of the
ImGui chrome, and the sound stays off so a hidden run never plays through the
human's speakers). Those need a person at the editor.

The editor runs hidden (HE_HIDDEN_WINDOW=1) with a private HOME and
HE_CONFIG_DIR under OUTDIR and a private copy of the tutorial project, so the
human's config, counters and project are never touched.

Usage:
    scripts/he_rewards_live.py OUTDIR [--editor PATH] [--timeout S]
"""

import datetime
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import he_mcp  # noqa: E402  — the real shim: same handshake, same framing
from he_mcp_multiclient import Client, PROJECT_SRC, expect, log  # noqa: E402

EDITOR = REPO / "out" / "deploy" / "Editor" / "HorizonEditor"


class Editor:
    """One editor process over a persistent private HOME/config (so a restart
    finds what the previous run wrote)."""

    def __init__(self, outdir, tag, editor_path, timeout):
        self.outdir = pathlib.Path(outdir)
        self.home = self.outdir / "home"
        self.cfgdir = self.outdir / "cfg"
        self.userdata = self.home / "Library" / "Application Support" / "HorizonEngine"
        self.endpoint = self.userdata / "mcp-endpoint.json"
        self.timeout = timeout
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()
        env = dict(os.environ)
        env["HOME"] = str(self.home)
        env["HE_CONFIG_DIR"] = str(self.cfgdir)
        env["HE_MCP"] = "1"
        env["HE_MCP_PORT"] = str(self.port)
        env.setdefault("HE_DUMP_RHI", "Metal")
        env.setdefault("HE_COLLAB_OFFLINE", "1")
        env.setdefault("HE_HIDDEN_WINDOW", "1")
        self.logpath = self.outdir / ("editor-%s.log" % tag)
        self.logfile = open(self.logpath, "wb")
        # HE_CONFIG_DIR outranks a config.json in the working directory
        # (GlobalState::configFilePath), so the deploy dir's own is ignored.
        self.proc = subprocess.Popen([str(editor_path)], env=env,
                                     stdout=self.logfile, stderr=subprocess.STDOUT,
                                     cwd=str(editor_path.parent))
        log("editor %s started, pid %d" % (tag, self.proc.pid))

    def wait_endpoint(self):
        t0 = time.time()
        while time.time() - t0 < self.timeout:
            if self.proc.poll() is not None:
                raise RuntimeError("editor exited early with rc %s" % self.proc.returncode)
            if self.endpoint.exists() and self.endpoint.stat().st_size > 0:
                try:
                    info = he_mcp.read_endpoint(str(self.endpoint))
                    if info["port"] == self.port:
                        log("endpoint up after %.0fs" % (time.time() - t0))
                        return
                except he_mcp.ShimError:
                    pass
            time.sleep(0.5)
        raise RuntimeError("no endpoint file after %ds" % self.timeout)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(20)
            except subprocess.TimeoutExpired:
                log("  editor ignored SIGTERM for 20 s, killing")
                self.proc.kill()
                self.proc.wait(5)
        self.logfile.close()
        log("editor stopped, rc %s" % self.proc.returncode)


def setup(outdir):
    cfgdir = outdir / "cfg"
    cfgdir.mkdir(parents=True)
    (outdir / "home").mkdir()
    project = outdir / "project"
    shutil.copytree(PROJECT_SRC, project)
    heproj = next(project.glob("*.heproj"))
    # RHI 4 = Metal, matching HE_DUMP_RHI (he-dump-rhi-ctx-backend-mismatch).
    write_cfg(outdir, {"LastProjectPath": str(heproj), "KnownProjects": [str(heproj)],
                       "RHI": 4, "CustomConfig": []})


def read_cfg(outdir):
    with open(outdir / "cfg" / "config.json") as f:
        return json.load(f)


def write_cfg(outdir, j):
    with open(outdir / "cfg" / "config.json", "w") as f:
        json.dump(j, f, indent=1)


def tally(outdir):
    """(day, buildsToday, streakDays) as the file has them; None = absent."""
    cc = {e["Key"]: e["Value"] for e in read_cfg(outdir).get("CustomConfig", [])
          if isinstance(e, dict) and "Key" in e}
    return (cc.get("RewardsDay"), cc.get("RewardsBuildsToday"), cc.get("RewardsStreakDays"))


def set_tally(outdir, day, builds, streak):
    j = read_cfg(outdir)
    keys = ("RewardsDay", "RewardsBuildsToday", "RewardsStreakDays")
    cc = [e for e in j.get("CustomConfig", [])
          if not (isinstance(e, dict) and e.get("Key") in keys)]
    cc += [{"Key": "RewardsDay", "Value": day},
           {"Key": "RewardsBuildsToday", "Value": builds},
           {"Key": "RewardsStreakDays", "Value": streak}]
    j["CustomConfig"] = cc
    write_cfg(outdir, j)


def export(c, outdir, tag, extra=None, wait_s=600):
    """One export run to its end. Returns the final status dict, or None if the
    tool refused to start one."""
    args = {"outputDir": str(outdir / "export" / tag), "incremental": False,
            "textureFormat": "None", "compress": False, "encrypt": False,
            "compileHorizonCode": False}
    args.update(extra or {})
    try:
        _, sc, _ = c.call("project_package", args)
    except RuntimeError as e:
        log("  %s: refused at start: %s" % (tag, e))
        return None
    log("  %s: started %s" % (tag, sc.get("started", sc)))
    t0 = time.time()
    while time.time() - t0 < wait_s:
        _, st, _ = c.call("project_build_status", {"log": False})
        if st.get("finished") and not st.get("running"):
            log("  %s: finished after %.1fs success=%s message=%s"
                % (tag, time.time() - t0, st.get("success"), st.get("message")))
            # The edge detector runs in the next UI frame, then the counters
            # are written through; give it many frames.
            time.sleep(2.0)
            return st
        time.sleep(0.5)
    raise RuntimeError("%s: export did not finish in %ds" % (tag, wait_s))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    editor_path = EDITOR
    timeout = 900
    for i, a in enumerate(args):
        if a == "--editor":
            editor_path = pathlib.Path(args[i + 1]).resolve()
        if a == "--timeout":
            timeout = int(args[i + 1])
    if not editor_path.exists():
        log("no editor at %s" % editor_path)
        return 2
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)
    setup(outdir)

    today = datetime.date.today()
    today_s = today.isoformat()
    yesterday_s = (today - datetime.timedelta(days=1)).isoformat()
    failures = []

    # ── Run 1 ────────────────────────────────────────────────────────────────
    log("── run 1: fresh config")
    ed = Editor(outdir, "run1", editor_path, timeout)
    try:
        ed.wait_endpoint()
        c = Client("A", ed.endpoint)
        time.sleep(3.0)
        t = tally(outdir)
        log("  tally before any moment: %s" % (t,))
        expect(t == (None, None, None), "nothing counted before the first moment", failures)

        st = export(c, outdir, "e1")
        expect(bool(st and st.get("success")), "export 1 succeeded", failures)
        t = tally(outdir)
        log("  tally: %s" % (t,))
        expect(t == (today_s, 1, 1), "export 1 → today, 1 build, streak 1", failures)

        time.sleep(3.0)   # many more frames polling the same finished run
        t = tally(outdir)
        expect(t == (today_s, 1, 1), "still 1 build after 3 s more of polling the same run", failures)

        st = export(c, outdir, "e2")
        expect(bool(st and st.get("success")), "export 2 succeeded", failures)
        t = tally(outdir)
        log("  tally: %s" % (t,))
        expect(t == (today_s, 2, 1), "export 2 → 2 builds", failures)

        # A failing run: no reward, no count. How it fails is up to the editor;
        # a target without a runtime bundle is the cheapest honest failure.
        st = export(c, outdir, "efail", {"targetPlatform": "Windows"})
        if st is None:
            log("  (failing export refused before a run started — nothing to observe)")
        else:
            expect(st.get("success") is False, "the Windows export without a runtime failed", failures)
            t = tally(outdir)
            log("  tally: %s" % (t,))
            expect(t == (today_s, 2, 1), "failed export → still 2", failures)

        _, sc, _ = c.call("settings_set", {"key": "rewards.enabled", "value": False})
        log("  settings_set rewards.enabled=false → %s" % sc)
        st = export(c, outdir, "eoff")
        expect(bool(st and st.get("success")), "export while off succeeded", failures)
        t = tally(outdir)
        log("  tally: %s" % (t,))
        expect(t == (today_s, 2, 1), "switched off → export not counted", failures)

        _, sc, _ = c.call("settings_set", {"key": "rewards.enabled", "value": True})
        log("  settings_set rewards.enabled=true → %s" % sc)
        time.sleep(3.0)
        t = tally(outdir)
        expect(t == (today_s, 2, 1), "switched back on → the run from the off phase is not rewarded late", failures)

        st = export(c, outdir, "e3")
        expect(bool(st and st.get("success")), "export 3 succeeded", failures)
        t = tally(outdir)
        log("  tally: %s" % (t,))
        expect(t == (today_s, 3, 1), "back on → export 3 → 3 builds", failures)
        c.close()
    finally:
        ed.stop()
    t = tally(outdir)
    expect(t == (today_s, 3, 1), "after shutdown the file still says 3", failures)

    # ── Run 2: restart ───────────────────────────────────────────────────────
    log("── run 2: restart")
    ed = Editor(outdir, "run2", editor_path, timeout)
    try:
        ed.wait_endpoint()
        c = Client("A", ed.endpoint)
        time.sleep(3.0)
        _, sc, _ = c.call("settings_get", {"key": "rewards.enabled"})
        log("  settings_get rewards.enabled → %s" % sc)
        st = export(c, outdir, "e4")
        expect(bool(st and st.get("success")), "export 4 succeeded", failures)
        t = tally(outdir)
        log("  tally: %s" % (t,))
        expect(t == (today_s, 4, 1), "after restart → 4 builds (continued, not reset)", failures)
        c.close()
    finally:
        ed.stop()

    # ── Run 3: the stored day is yesterday ───────────────────────────────────
    log("── run 3: day rollover (RewardsDay = %s by hand)" % yesterday_s)
    set_tally(outdir, yesterday_s, 7, 1)
    ed = Editor(outdir, "run3", editor_path, timeout)
    try:
        ed.wait_endpoint()
        c = Client("A", ed.endpoint)
        time.sleep(3.0)
        t = tally(outdir)
        expect(t == (yesterday_s, 7, 1), "starting the editor alone counts nothing", failures)
        st = export(c, outdir, "e5")
        expect(bool(st and st.get("success")), "export 5 succeeded", failures)
        t = tally(outdir)
        log("  tally: %s" % (t,))
        expect(t == (today_s, 1, 2), "yesterday → today: 1 build today, 2 days in a row", failures)
        c.close()
    finally:
        ed.stop()

    log("RESULT %s (%d failure(s))" % ("PASS" if not failures else "FAIL", len(failures)))
    for f in failures:
        log("  failed: %s" % f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
