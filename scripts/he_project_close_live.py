#!/usr/bin/env python3
"""he_project_close_live — closing a project stops what it ran; a pin edit plays no sound.

Drives the deployed HorizonEditor hidden on a COPY of an application project whose
GameInstance OnInit plays a looping sound (Tamagochi is one). Three runs, the
witness is the Audio trace (HE_LOG=Audio=Trace) and the shutdown line
"Audio engine shutting down (N active voice(s), ...)":

  close   HE_CLOSE_PROJECT_AT_FRAME: File > Close Project at frame 200, leave at 400.
          The preview's OnShutdown runs before the world is cleared, 0 voices at exit.
  reopen  ...and HE_REOPEN_PROJECT_AT_FRAME=300: the preview starts again, 1 voice.
  pin     Remote Control (MCP) hc_set_pin_default on the GameInstance's audio.play
          node: clear the asset, then set it back. Each is a commit that rebuilds
          the preview; neither may start a sound, 0 voices at exit.

  he_project_close_live.py <project.heproj> [--editor-dir out/deploy/Editor] [--out DIR]

Docs: docs/project-close-teardown-2026-11-25.md, docs/headless-runs.md.
"""
import argparse, json, os, pathlib, re, shutil, socket, subprocess, sys, tempfile, time

ap = argparse.ArgumentParser()
ap.add_argument("project")
ap.add_argument("--editor-dir", default=str(pathlib.Path(__file__).resolve().parent.parent / "out/deploy/Editor"))
ap.add_argument("--out", default=None)
a = ap.parse_args()

editor_dir = pathlib.Path(a.editor_dir)
sys.path.insert(0, str(editor_dir))
import he_mcp  # noqa: E402  (shipped next to the editor)

out = pathlib.Path(a.out or tempfile.mkdtemp(prefix="he_close_live_"))
src = pathlib.Path(a.project).resolve()
proj = out / "proj" / src.parent.name
shutil.rmtree(proj, ignore_errors=True)
shutil.copytree(src.parent, proj)
heproj = proj / src.name


def run(tag, extra, during=None):
    """One hidden editor run; returns its log text. `during(endpoint)` runs while it is up."""
    home = out / ("home_" + tag)
    shutil.rmtree(home, ignore_errors=True)
    userdata = home / "Library" / "Application Support" / "HorizonEngine"
    userdata.mkdir(parents=True)
    cfg = out / ("cfg_" + tag)
    cfg.mkdir(exist_ok=True)
    # A close writes LastProjectPath "" — every run starts from its own config.
    (cfg / "config.json").write_text(json.dumps({"RHI": 4, "LastProjectPath": str(heproj),
                                                 "KnownProjects": [str(heproj)]}))
    s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
    env = dict(os.environ, HOME=str(home), HE_CONFIG_DIR=str(cfg), HE_HIDDEN_WINDOW="1",
               HE_COLLAB_OFFLINE="1", HE_SKIP_PROJECT_PREFLIGHT="1", HE_LOG="Audio=Trace",
               HE_MCP="1", HE_MCP_PORT=str(port), **extra)
    logpath = out / (tag + ".log")
    with open(logpath, "wb") as log:
        ed = subprocess.Popen(["./HorizonEditor"], cwd=editor_dir, env=env,
                              stdout=log, stderr=subprocess.STDOUT)
        if during:
            during(userdata / "mcp-endpoint.json", ed)
        ed.wait(timeout=600)
    return logpath.read_text(errors="replace")


def pin_edits(endpoint, ed):
    t0 = time.time()
    while not (endpoint.exists() and endpoint.stat().st_size > 0):
        if ed.poll() is not None or time.time() - t0 > 120:
            raise SystemExit("no Remote Control endpoint")
        time.sleep(0.5)
    b = he_mcp.Bridge(str(endpoint)); b.ensure()
    call = lambda tool, args: b.request("tools/call", {"name": tool, "arguments": args})["result"]
    graph = call("hc_get", {"key": "gameinstance"}).get("structuredContent") or {}
    node = next(n for n in graph.get("nodes", []) if n.get("s") == "audio.play")
    clip = next(p["v"] for p in node.get("pinDefaults", []) if p.get("i") == 0)
    for value in (None, clip):
        call("hc_set_pin_default", {"key": "gameinstance", "node": node["id"], "pin": "asset", "value": value})
        time.sleep(1.0)


def voices(text):
    m = re.findall(r"Audio engine shutting down \((\d+) active voice", text)
    return int(m[-1]) if m else None


ok = True
def check(name, cond, detail):
    global ok
    ok &= bool(cond)
    print("%-4s %s (%s)" % ("PASS" if cond else "FAIL", name, detail))

t = run("close", {"HE_CLOSE_PROJECT_AT_FRAME": "200", "HE_EXIT_AFTER_FRAMES": "400"})
shut, clear = t.find("GameInstance OnShutdown"), t.find("Clearing world", t.find("starting the live preview"))
check("close: no voice survives", voices(t) == 0, "voices=%s" % voices(t))
check("close: OnShutdown before the world goes", 0 <= shut < clear, "at %d, world cleared at %d" % (shut, clear))

t = run("reopen", {"HE_CLOSE_PROJECT_AT_FRAME": "200", "HE_REOPEN_PROJECT_AT_FRAME": "300",
                   "HE_EXIT_AFTER_FRAMES": "500"})
starts = t.count("starting the live preview")
check("reopen: the preview starts again", starts == 2 and voices(t) == 1, "starts=%d voices=%s" % (starts, voices(t)))

t = run("pin", {"HE_EXIT_AFTER_FRAMES": "4000"}, during=pin_edits)
restarts, sounds = t.count("live preview restarted"), t.count("Started 2D")
check("pin: an edit plays nothing", restarts == 2 and sounds == 1 and voices(t) == 0,
      "restarts=%d sounds started=%d voices=%s" % (restarts, sounds, voices(t)))

print("logs in", out)
sys.exit(0 if ok else 1)
