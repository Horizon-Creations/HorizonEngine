#!/usr/bin/env python3
"""he_rewards_live — the reward feedback (Themen 75 + 95) against a live editor.

Drives the deployed HorizonEditor through its Remote Control bridge and checks
what can be checked WITHOUT a screen, a keyboard and a speaker: the
BuildSucceeded moment, the persistent progress counters behind the footer's
"Ready · N builds today · N days in a row" (EditorRewards.h), every switch of
Preferences ▸ Feedback, and — through the Audio trace log — whether the
editor's own UI-sound engine opens, closes and starts a tone.

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
  Run 4  the switches (Thema 95):
                         every rewards.* setting reads its documented default;
                         baseline = master on, every other switch off; then
                         EACH switch on alone: settings_get says so, the file
                         says so, an export is still counted, and only
                         "Success Sound" opens the UI-sound engine (the four
                         per-tone switches alone open nothing — they are under
                         it); Mute Editor Sounds alone opens nothing;
                         Reduced Motion in both states;
                         the tones: chime on a finished export, the failed
                         tone on a failing one, neither with its own switch
                         off, none at volume 0, none muted (the engine closes),
                         none with the master off (the engine closes, the
                         export is not counted);
                         ALL off (master too): nothing counted, no engine;
                         ALL on: counted, engine open, chime.
  Run 5  restart:        every value of "all on" survived the restart;
                         Restore Defaults by hand → the file says so.

The tones play at rewards.volume 0.01 (gain 1e-4, about -80 dB) and the volume
is always set BEFORE rewards.sound goes on: the hidden editor is unfocused, so
the build tones really play, through whatever output this machine has. The
witness is the Audio trace line "Started 2D PCM sound #N: F frames …" of the
UI engine (HE_LOG=Audio=Trace) — F tells the chime from the failed tone.

NOT checked here, and not checkable this way: Saved (scene_save bypasses the
reward on purpose — an agent's save is not the user's), Assets imported (no
MCP tool imports), and everything that is only a picture or a sound — the
footer line, the check mark, the light edge, the tab check, the import frame,
the counter tick, the tooltip, what Reduced Motion changes on screen, and how
the tones sound. The switches for those are checked for reaching the editor
and the file, and for not disturbing the counting; their effect needs a person
at the editor.

The editor runs hidden (HE_HIDDEN_WINDOW=1) with a private HOME (APPDATA on
Windows) and HE_CONFIG_DIR under OUTDIR and a private copy of the tutorial
project, so the human's config, counters and project are never touched.

Usage:
    scripts/he_rewards_live.py OUTDIR [--editor PATH] [--project DIR]
                                      [--timeout S] [--only-new]
    --only-new: skip runs 1-3 (Thema 75) and run 4-5 on a fresh config.
"""

import datetime
import json
import os
import pathlib
import re
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

IS_WIN = sys.platform == "win32"
EDITOR = REPO / "out" / "deploy" / "Editor" / ("HorizonEditor.exe" if IS_WIN else "HorizonEditor")
# RendererBackend (Types/Enums.h): 0 = OpenGL, 4 = Metal. Config and HE_DUMP_RHI
# must agree (he-dump-rhi-ctx-backend-mismatch).
RHI_NAME, RHI_INDEX = ("OpenGL", 0) if IS_WIN else ("Metal", 4)
# A target without a prebuilt runtime bundle: the cheapest honest failure. Not
# the host's own platform, or it would succeed.
FAIL_PLATFORM = "Linux" if IS_WIN else "Windows"
# Where the tutorial project lives when ~/Documents is synced elsewhere.
PROJECT_FALLBACKS = [pathlib.Path.home() / "iCloudDrive" / "Documents" / "HorizonEngine" / "HorizonTutorial"]

# ── Preferences ▸ Feedback (EditorSettingsCatalog.cpp) ───────────────────────
# catalog key → (config.json CustomConfig key, documented default)
SETTINGS = {
    "rewards.enabled":          ("RewardsEnabled", True),
    "rewards.visual":           ("RewardsVisual", True),
    "rewards.checkMark":        ("RewardsCheckMark", True),
    "rewards.lightEdge":        ("RewardsLightEdge", True),
    "rewards.momentCompile":    ("RewardsMomentCompile", True),
    "rewards.momentCommit":     ("RewardsMomentCommit", True),
    "rewards.momentTutorial":   ("RewardsMomentTutorial", True),
    "rewards.tabCheck":         ("RewardsTabCheck", True),
    "rewards.importHighlight":  ("RewardsImportHighlight", True),
    "rewards.reducedMotion":    ("RewardsReducedMotion", 0),
    "rewards.sound":            ("RewardsSound", False),
    "rewards.volume":           ("RewardsVolume", 0.5),
    "rewards.soundSave":        ("RewardsSoundSave", True),
    "rewards.soundBuild":       ("RewardsSoundBuild", True),
    "rewards.soundBuildFailed": ("RewardsSoundBuildFailed", True),
    "rewards.soundImport":      ("RewardsSoundImport", True),
    "rewards.showProgress":     ("RewardsShowProgress", True),
    "rewards.counterTick":      ("RewardsCounterTick", True),
    "rewards.streakTooltip":    ("RewardsStreakTooltip", True),
    "rewards.muteEditorSounds": ("EditorSoundsMuted", False),
}
# The on/off switches under the master, in catalog order.
SUB_SWITCHES = [k for k, (_, d) in SETTINGS.items()
                if isinstance(d, bool) and k != "rewards.enabled"]
QUIET = 0.01   # rewards.volume for every tone witness: gain 1e-4


class Editor:
    """One editor process over a persistent private HOME/config (so a restart
    finds what the previous run wrote)."""

    def __init__(self, outdir, tag, editor_path, timeout):
        self.outdir = pathlib.Path(outdir)
        self.home = self.outdir / "home"
        self.cfgdir = self.outdir / "cfg"
        if IS_WIN:
            self.appdata = self.home / "AppData" / "Roaming"
            self.userdata = self.appdata / "HorizonEngine"
        else:
            self.userdata = self.home / "Library" / "Application Support" / "HorizonEngine"
        self.endpoint = self.userdata / "mcp-endpoint.json"
        self.timeout = timeout
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()
        env = dict(os.environ)
        env["HOME"] = str(self.home)
        if IS_WIN:
            env["APPDATA"] = str(self.appdata)   # GlobalState::userDataDir on Windows
        env["HE_CONFIG_DIR"] = str(self.cfgdir)
        env["HE_MCP"] = "1"
        env["HE_MCP_PORT"] = str(self.port)
        env.setdefault("HE_DUMP_RHI", RHI_NAME)
        env.setdefault("HE_COLLAB_OFFLINE", "1")
        env.setdefault("HE_HIDDEN_WINDOW", "1")
        # The tone witness: AudioEngine::startSound traces every voice it starts.
        env.setdefault("HE_LOG", "Audio=Trace")
        self.logpath = self.outdir / ("editor-%s.log" % tag)
        self.logfile = open(self.logpath, "wb")
        # The log FILE, not stdout: Log.cpp flushes the file per record, while
        # stdout into a pipe is block-buffered and trails by whole exports.
        # Next to the exe, or in the user data dir where that is read-only;
        # rewritten on every start (GlobalState.cpp).
        self.filelog_candidates = [editor_path.parent / "HorizonEngine.log",
                                   self.userdata / "HorizonEngine.log"]
        self.filelog_copy = self.outdir / ("editor-%s.file.log" % tag)
        self.started_at = time.time()
        # HE_CONFIG_DIR outranks a config.json in the working directory
        # (GlobalState::configFilePath), so the deploy dir's own is ignored.
        self.proc = subprocess.Popen([str(editor_path)], env=env,
                                     stdout=self.logfile, stderr=subprocess.STDOUT,
                                     cwd=str(editor_path.parent))
        log("editor %s started, pid %d" % (tag, self.proc.pid))

    def filelog(self):
        for p in self.filelog_candidates:
            if p.exists() and p.stat().st_mtime >= self.started_at - 1.0:
                return p
        return None

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

    def alive(self):
        return self.proc.poll() is None

    def text(self):
        p = self.filelog()
        if p is None:
            raise RuntimeError("no HorizonEngine.log written since the start (%s)"
                               % ", ".join(map(str, self.filelog_candidates)))
        with open(p, "rb") as f:
            return f.read().decode("utf-8", "replace")

    def stop(self):
        if self.proc.poll() is None:
            if IS_WIN:
                # terminate() is TerminateProcess there: no exit path at all. Ask
                # the (hidden) window to close first, like the close box would.
                closed = _close_windows_of(self.proc.pid)
                log("  WM_CLOSE sent to %d window(s)" % closed)
                try:
                    self.proc.wait(20)
                    log("  editor closed on WM_CLOSE")
                except subprocess.TimeoutExpired:
                    log("  editor ignored WM_CLOSE for 20 s, killing")
                    self.proc.kill()
                    self.proc.wait(5)
            else:
                self.proc.terminate()
                try:
                    self.proc.wait(20)
                except subprocess.TimeoutExpired:
                    log("  editor ignored SIGTERM for 20 s, killing")
                    self.proc.kill()
                    self.proc.wait(5)
        self.logfile.close()
        p = self.filelog()
        if p is not None:
            shutil.copy(p, self.filelog_copy)
        log("editor stopped, rc %s" % self.proc.returncode)


def _close_windows_of(pid):
    """Post WM_CLOSE to every top-level window of pid, hidden ones included."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    found = []
    proto = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            found.append(hwnd)
        return True
    user32.EnumWindows(proto(cb), 0)
    for h in found:
        user32.PostMessageW(h, 0x0010, 0, 0)   # WM_CLOSE
    return len(found)


def system_reduces_motion():
    """What "Follow System" means on this machine (EditorSystemMotion.cpp),
    or None where the script cannot ask."""
    if not IS_WIN:
        return None
    import ctypes
    from ctypes import wintypes
    animate = wintypes.BOOL(1)
    SPI_GETCLIENTAREAANIMATION = 0x1042
    if not ctypes.windll.user32.SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0,
                                                       ctypes.byref(animate), 0):
        return None
    return not animate.value


def setup(outdir, project_src):
    cfgdir = outdir / "cfg"
    cfgdir.mkdir(parents=True)
    (outdir / "home").mkdir()
    project = outdir / "project"
    shutil.copytree(project_src, project)
    heproj = next(project.glob("*.heproj"))
    write_cfg(outdir, {"LastProjectPath": str(heproj), "KnownProjects": [str(heproj)],
                       "RHI": RHI_INDEX, "CustomConfig": []})


def read_cfg(outdir):
    with open(outdir / "cfg" / "config.json") as f:
        return json.load(f)


def write_cfg(outdir, j):
    with open(outdir / "cfg" / "config.json", "w") as f:
        json.dump(j, f, indent=1)


def custom(outdir):
    return {e["Key"]: e["Value"] for e in read_cfg(outdir).get("CustomConfig", [])
            if isinstance(e, dict) and "Key" in e}


def tally(outdir):
    """(day, buildsToday, streakDays) as the file has them; None = absent."""
    cc = custom(outdir)
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


# ── Thema 95: switches and the UI-sound engine ───────────────────────────────

class Audio:
    """The Audio log lines of one editor run, counted from a mark on. The
    project's engine opens once at startup; every further "ready" is the
    UI-sound engine opening (keepUiAudio), every "shutting down" before exit
    is it closing, and every "Started" is a voice — the project's engine plays
    nothing in an idle editor, so a voice here is a feedback tone."""

    READY = "Audio engine ready"
    DOWN = re.compile(r"Audio engine shutting down \((\d+) active voice")
    STARTED = re.compile(r"Started 2D PCM sound #(\d+): (\d+) frames, (\d+) Hz, (\d+) ch, vol ([0-9.]+)")

    def __init__(self, ed):
        self.ed = ed

    def counts(self):
        t = self.ed.text()
        return {"ready": t.count(self.READY),
                "down": [int(m.group(1)) for m in self.DOWN.finditer(t)],
                "started": [(int(m.group(2)), float(m.group(5))) for m in self.STARTED.finditer(t)]}

    def is_open(self):
        c = self.counts()
        # 1 ready = the project engine; each later ready/down pair is the UI one.
        return c["ready"] - 1 > len(c["down"])

    def wait_open(self, want, wait_s=6.0):
        t0 = time.time()
        while time.time() - t0 < wait_s:
            if self.is_open() == want:
                return True
            time.sleep(0.25)
        return self.is_open() == want


REDUCED_MOTION_LABELS = ["Follow System", "Off"]   # the enumRow's options, by index


def s_get(c, key):
    """The value as the file stores it: an enum row answers with its label."""
    _, sc, _ = c.call("settings_get", {"key": key})
    v = sc.get("value", sc)
    if key == "rewards.reducedMotion" and v in REDUCED_MOTION_LABELS:
        return REDUCED_MOTION_LABELS.index(v)
    return v


def s_set(c, key, value):
    _, sc, _ = c.call("settings_set", {"key": key, "value": value})
    return sc


def same(a, b):
    if isinstance(b, float) or isinstance(a, float):
        try:
            return abs(float(a) - float(b)) < 1e-4
        except (TypeError, ValueError):
            return False
    return a == b


def check_setting(c, outdir, key, value, failures, where=""):
    """settings_set, then settings_get and the file agree."""
    sc = s_set(c, key, value)
    got = s_get(c, key)
    cfg_key = SETTINGS[key][0]
    in_file = custom(outdir).get(cfg_key)
    ok = same(got, value) and same(in_file, value)
    expect(ok, "%s%s = %s → settings_get %s, file %s=%s (persisted=%s)"
           % (where, key, value, got, cfg_key, in_file, sc.get("persisted")), failures)


def set_all(c, values):
    for k, v in values.items():
        s_set(c, k, v)


def witness_export(c, outdir, audio, tag, failures, *, counted, tone, fail=False,
                   ui_open=None):
    """One export after the tone gap, then: counted or not, how many tones
    started (0 or 1) and whether the UI engine is open. Returns the frames of
    the tone that started, or None."""
    time.sleep(2.5)   # kToneGapSec: a tone right after the last one is swallowed
    before_b = tally(outdir)[1] or 0
    before = audio.counts()
    st = export(c, outdir, tag, {"targetPlatform": FAIL_PLATFORM} if fail else None)
    after = audio.counts()
    after_b = tally(outdir)[1] or 0
    new = after["started"][len(before["started"]):]
    if fail:
        if st is None:
            log("  %s: the failing export was refused before a run started — no "
                "BuildFailed edge to witness" % tag)
            expect(False, "%s: a failing export that actually runs" % tag, failures)
            return None
        expect(st.get("success") is False, "%s: the %s export without a runtime failed"
               % (tag, FAIL_PLATFORM), failures)
        expect(after_b == before_b, "%s: failed → not counted (%s → %s)"
               % (tag, before_b, after_b), failures)
    else:
        expect(bool(st and st.get("success")), "%s: export succeeded" % tag, failures)
        want_b = before_b + 1 if counted else before_b
        expect(after_b == want_b, "%s: %s (%s → %s)"
               % (tag, "counted" if counted else "not counted", before_b, after_b), failures)
    expect(len(new) == (1 if tone else 0), "%s: %s (voices started: %s)"
           % (tag, "one tone" if tone else "no tone", new), failures)
    if ui_open is not None:
        expect(audio.is_open() == ui_open, "%s: UI-sound engine %s"
               % (tag, "open" if ui_open else "closed"), failures)
    return new[0][0] if new else None


def run_switches(ed, c, outdir, failures):
    audio = Audio(ed)
    log("  system reduces motion (what 'Follow System' follows here): %s"
        % system_reduces_motion())

    # Defaults, as a fresh (or Thema-75-exercised) config has them.
    for k, (_, d) in SETTINGS.items():
        got = s_get(c, k)
        expect(same(got, d), "default %s = %s (got %s)" % (k, d, got), failures)
    expect(audio.counts()["ready"] == 1 and not audio.is_open(),
           "sound off by default → only the project's engine opened", failures)

    # The volume first, before anything could play.
    check_setting(c, outdir, "rewards.volume", QUIET, failures)

    # Baseline: master on, every other switch off.
    base = {k: False for k in SUB_SWITCHES}
    set_all(c, base)
    for k in SUB_SWITCHES:
        expect(custom(outdir).get(SETTINGS[k][0]) is False, "baseline: %s off in the file" % k,
               failures)
    witness_export(c, outdir, audio, "b0", failures, counted=True, tone=False, ui_open=False)

    # Each switch on, alone.
    for k in SUB_SWITCHES:
        log("── %s on, alone" % k)
        check_setting(c, outdir, k, True, failures, "alone: ")
        opens = (k == "rewards.sound")
        expect(audio.wait_open(opens), "alone: %s → UI-sound engine %s"
               % (k, "opens" if opens else "stays closed"), failures)
        # rewards.sound alone: the engine is open but no tone has its switch on.
        witness_export(c, outdir, audio, "one-" + k.split(".")[1], failures,
                       counted=True, tone=False, ui_open=opens)
        s_set(c, k, False)
        expect(audio.wait_open(False), "alone: %s off again → engine closed" % k, failures)

    # Reduced Motion, both states.
    for v in (1, 0):
        log("── rewards.reducedMotion = %d" % v)
        check_setting(c, outdir, "rewards.reducedMotion", v, failures)
        witness_export(c, outdir, audio, "rm%d" % v, failures, counted=True, tone=False)

    # The tones.
    log("── tones (volume %.2f)" % QUIET)
    s_set(c, "rewards.soundBuild", True)
    s_set(c, "rewards.soundBuildFailed", True)
    s_set(c, "rewards.sound", True)
    expect(audio.wait_open(True), "sound on → UI-sound engine opens before any moment", failures)
    chime = witness_export(c, outdir, audio, "t-chime", failures, counted=True, tone=True,
                           ui_open=True)
    failed = witness_export(c, outdir, audio, "t-failed", failures, counted=False, tone=True,
                            fail=True, ui_open=True)
    log("  chime %s frames, failed tone %s frames" % (chime, failed))
    if chime and failed:
        expect(chime != failed, "the failed tone is not the chime", failures)
    vols = [v for _, v in audio.counts()["started"]]
    expect(all(v < 0.01 for v in vols), "every tone at gain < 0.01 (%s)" % vols, failures)

    s_set(c, "rewards.soundBuild", False)
    witness_export(c, outdir, audio, "t-nobuild", failures, counted=True, tone=False, ui_open=True)
    s_set(c, "rewards.soundBuild", True)
    s_set(c, "rewards.soundBuildFailed", False)
    witness_export(c, outdir, audio, "t-nofailed", failures, counted=False, tone=False,
                   fail=True, ui_open=True)
    s_set(c, "rewards.soundBuildFailed", True)

    s_set(c, "rewards.volume", 0.0)
    witness_export(c, outdir, audio, "t-vol0", failures, counted=True, tone=False, ui_open=True)
    s_set(c, "rewards.volume", QUIET)

    downs = len(audio.counts()["down"])
    check_setting(c, outdir, "rewards.muteEditorSounds", True, failures, "tones: ")
    expect(audio.wait_open(False), "muted → the UI-sound engine closes", failures)
    d = audio.counts()["down"]
    if len(d) > downs:
        log("  UI engine closed with %d voice(s) still held (tones started so far: %d)"
            % (d[-1], len(audio.counts()["started"])))
    witness_export(c, outdir, audio, "t-muted", failures, counted=True, tone=False, ui_open=False)
    s_set(c, "rewards.muteEditorSounds", False)
    expect(audio.wait_open(True), "unmuted → it opens again", failures)

    s_set(c, "rewards.enabled", False)
    expect(audio.wait_open(False), "master off → the UI-sound engine closes", failures)
    witness_export(c, outdir, audio, "t-master-off", failures, counted=False, tone=False,
                   ui_open=False)
    s_set(c, "rewards.enabled", True)
    expect(audio.wait_open(True), "master on → it opens again", failures)
    time.sleep(1.0)
    expect(len(audio.counts()["started"]) == 2,
           "master back on → the run from the off phase gets no late tone", failures)
    s_set(c, "rewards.sound", False)
    expect(audio.wait_open(False), "sound off → the UI-sound engine closes", failures)

    # All off, the master too.
    log("── all off")
    check_setting(c, outdir, "rewards.volume", 0.0, failures, "all off: ")
    set_all(c, {k: False for k in SUB_SWITCHES})
    check_setting(c, outdir, "rewards.enabled", False, failures, "all off: ")
    witness_export(c, outdir, audio, "all-off", failures, counted=False, tone=False,
                   ui_open=False)

    # All on (volume first).
    log("── all on")
    check_setting(c, outdir, "rewards.volume", QUIET, failures, "all on: ")
    for k in SUB_SWITCHES:
        if k != "rewards.muteEditorSounds":
            s_set(c, k, True)
    check_setting(c, outdir, "rewards.enabled", True, failures, "all on: ")
    expect(audio.wait_open(True), "all on → UI-sound engine open", failures)
    witness_export(c, outdir, audio, "all-on", failures, counted=True, tone=True, ui_open=True)
    expect(ed.alive(), "the editor is still running after the sweep", failures)
    errors = [l for l in ed.text().splitlines() if "[ERROR]" in l or "[FATAL]" in l]
    log("  %d ERROR/FATAL line(s) in this run's log" % len(errors))
    for l in errors[:20]:
        log("    " + l[:200])


ALL_ON = dict({k: True for k in SUB_SWITCHES}, **{
    "rewards.enabled": True, "rewards.muteEditorSounds": False,
    "rewards.volume": QUIET, "rewards.reducedMotion": 0})


def run_restart_values(ed, c, outdir, failures):
    for k, v in ALL_ON.items():
        got = s_get(c, k)
        expect(same(got, v), "after restart %s = %s (got %s)" % (k, v, got), failures)
    audio = Audio(ed)
    expect(audio.wait_open(True), "after restart with sound on → UI-sound engine opens", failures)
    # Restore Defaults, the way the panel's button leaves the file: the volume
    # goes back to 0.5, so switch sound off first.
    s_set(c, "rewards.sound", False)
    for k, (_, d) in SETTINGS.items():
        s_set(c, k, d)
    cc = custom(outdir)
    for k, (ck, d) in SETTINGS.items():
        expect(same(cc.get(ck), d), "restored default in the file: %s=%s (got %s)"
               % (ck, d, cc.get(ck)), failures)
    expect(audio.wait_open(False), "defaults (sound off) → UI-sound engine closed", failures)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    editor_path = EDITOR
    project_src = PROJECT_SRC
    timeout = 900
    only_new = "--only-new" in args
    for i, a in enumerate(args):
        if a == "--editor":
            editor_path = pathlib.Path(args[i + 1]).resolve()
        if a == "--project":
            project_src = pathlib.Path(args[i + 1]).resolve()
        if a == "--timeout":
            timeout = int(args[i + 1])
    if not editor_path.exists():
        log("no editor at %s" % editor_path)
        return 2
    if not project_src.exists():
        project_src = next((p for p in PROJECT_FALLBACKS if p.exists()), project_src)
    if not project_src.exists():
        log("no tutorial project at %s (--project DIR)" % project_src)
        return 2
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)
    setup(outdir, project_src)

    today = datetime.date.today()
    today_s = today.isoformat()
    yesterday_s = (today - datetime.timedelta(days=1)).isoformat()
    failures = []

    if not only_new:
        run_counters(outdir, editor_path, timeout, today_s, yesterday_s, failures)

    # ── Run 4: the switches ─────────────────────────────────────────────────
    log("── run 4: the switches (Thema 95)")
    ed = Editor(outdir, "run4", editor_path, timeout)
    try:
        ed.wait_endpoint()
        c = Client("A", ed.endpoint)
        time.sleep(3.0)
        run_switches(ed, c, outdir, failures)
        c.close()
    finally:
        ed.stop()

    # ── Run 5: restart, then Restore Defaults ───────────────────────────────
    log("── run 5: restart with everything on")
    ed = Editor(outdir, "run5", editor_path, timeout)
    try:
        ed.wait_endpoint()
        c = Client("A", ed.endpoint)
        time.sleep(3.0)
        run_restart_values(ed, c, outdir, failures)
        c.close()
    finally:
        ed.stop()

    log("RESULT %s (%d failure(s))" % ("PASS" if not failures else "FAIL", len(failures)))
    for f in failures:
        log("  failed: %s" % f)
    return 0 if not failures else 1


def run_counters(outdir, editor_path, timeout, today_s, yesterday_s, failures):
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

        # A failing run: no reward, no count.
        st = export(c, outdir, "efail", {"targetPlatform": FAIL_PLATFORM})
        if st is None:
            log("  (failing export refused before a run started — nothing to observe)")
        else:
            expect(st.get("success") is False, "the %s export without a runtime failed"
                   % FAIL_PLATFORM, failures)
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


if __name__ == "__main__":
    sys.exit(main())
