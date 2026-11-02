#!/usr/bin/env python3
"""he_export_step_order_live — the export's rings fill in the order they are listed (topic 222 step 6).

Starts the deployed HorizonEditor (out/deploy/Editor) hidden, with a private
HOME/config and a copy of the tutorial project, and exports it twice through
the Remote Control bridge (`project_package`, Development, target Host) while
polling `project_build_status` as fast as the bridge answers:

  run "plain"   compileHorizonCode off:  Setup, Build game, Package, Clean up
  run "hc"      a GameInstance graph authored over MCP first, compileHorizonCode
                on:  Setup, Build game, Build HorizonCode, Package, Clean up
  run "enc"     as "hc", pak encrypted: the key compiled into the game was
                planned before the pack, so the game must still open its pak
                (start the exported game to see it: "mounted <name>.hpak")

For every run it checks that the step list is exactly that, that the steps
started in list order (each one only once every step above it had stopped
running), that none finished before a step above it and that the export
succeeded with "Game compiled from the SDK (dev)" in the log. The timeline
(every change of the step states, with the time) goes to OUTDIR/timeline-<run>.json,
and the first poll with "Build game" running and a later step still pending
goes to OUTDIR/midrun-<run>.json, which tests/test_build_footer_ui.cpp draws
when HE_EXPORT_STEPS_SNAPSHOT names it (the picture of the step display).

  usage: he_export_step_order_live.py OUTDIR [--editor PATH]

Exit code 0 when every expectation above held.
"""
import json
import os
import pathlib
import shutil
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import he_rewards_live as R  # noqa: E402  — Editor, setup
from he_mcp_multiclient import Client, PROJECT_SRC, log  # noqa: E402

EXPECTED = {
    "plain": ["Setup", "Build game", "Package", "Clean up"],
    "hc":    ["Setup", "Build game", "Build HorizonCode", "Package", "Clean up"],
    "enc":   ["Setup", "Build game", "Build HorizonCode", "Package", "Clean up"],
}


def author_horizon_code(c):
    """One variable and one node in the GameInstance graph, saved: something to compile."""
    c.call("hc_add_variable", {"key": "gameinstance", "name": "Lives", "type": "Int"})
    c.call("hc_add_node", {"key": "gameinstance", "type": "Branch", "position": [40, 200]})
    c.call("hc_save", {"key": "gameinstance"})


def run_export(c, outdir, tag, hc, encrypt=False):
    args = {"outputDir": str(outdir / "export" / tag), "incremental": False,
            "textureFormat": "None", "compress": False, "encrypt": encrypt,
            "compileHorizonCode": hc, "profile": "Development"}
    t0 = time.time()
    while True:
        try:
            c.call("project_package", args)
            break
        except RuntimeError as e:
            if "still checking" in str(e) and time.time() - t0 < 120:
                time.sleep(1.0)
                continue
            raise
    t0 = time.time()
    timeline, last, midrun, st = [], None, None, None
    while time.time() - t0 < 900:
        _, st, _ = c.call("project_build_status", {"log": False})
        states = [(s["name"], s["state"]) for s in st.get("steps", [])]
        if states != last:
            timeline.append({"t": round(time.time() - t0, 2), "steps": states,
                             "activity": st.get("activity", "")})
            last = states
        names = [n for n, _ in states]
        if (midrun is None and "Build game" in names
                and dict(states).get("Build game") == "running"
                and any(s["state"] == "pending" for s in st["steps"][names.index("Build game") + 1:])
                and any(s.get("progress", 0) > 0.2 for s in st["steps"] if s["name"] == "Build game")):
            midrun = {"steps": st["steps"], "activity": st.get("activity", "")}
        if st.get("finished") and not st.get("running"):
            break
        time.sleep(0.05)
    (outdir / ("timeline-%s.json" % tag)).write_text(json.dumps(timeline, indent=1))
    if midrun:
        (outdir / ("midrun-%s.json" % tag)).write_text(json.dumps(midrun, indent=1))
    _, full, _ = c.call("project_build_status", {"log": True, "maxLines": 2000})
    lines = [l.get("text", "") if isinstance(l, dict) else str(l) for l in full.get("log", [])]
    (outdir / ("build-%s.log" % tag)).write_text("\n".join(lines))
    return st, timeline, midrun, lines


def check_order(tag, st, timeline, lines):
    ok = True

    def expect(cond, what):
        nonlocal ok
        log("  %s %s" % ("ok  " if cond else "FAIL", what))
        ok = ok and cond

    names = [n for n, _ in timeline[-1]["steps"]] if timeline else []
    expect(names == EXPECTED[tag], "%s: step list %s" % (tag, names))
    # When each step was first seen running / over, as an index into the timeline.
    started, ended = {}, {}
    for i, snap in enumerate(timeline):
        for n, s in snap["steps"]:
            if s != "pending" and n not in started:
                started[n] = i
            if s in ("done", "failed") and n not in ended:
                ended[n] = i
        running = [n for n, s in snap["steps"] if s == "running"]
        if len(running) > 1:
            expect(False, "%s: more than one ring running at t=%.2f %s" % (tag, snap["t"], running))
        # Nothing below a pending step may have started.
        seen_pending = False
        for n, s in snap["steps"]:
            if s == "pending":
                seen_pending = True
            elif seen_pending:
                expect(False, "%s: %s is %s while a step above it is still pending (t=%.2f)"
                       % (tag, n, s, snap["t"]))
    order = sorted(started, key=lambda n: started[n])
    expect(order == [n for n in names if n in started], "%s: rings started in list order %s" % (tag, order))
    fin = sorted(ended, key=lambda n: ended[n])
    expect(fin == [n for n in names if n in ended], "%s: rings finished in list order %s" % (tag, fin))
    expect(bool(st.get("success")), "%s: export succeeded (%s)" % (tag, st.get("message")))
    expect(any("Game compiled from the SDK (dev)" in l for l in lines),
           "%s: 'Game compiled from the SDK (dev)' in the log" % tag)
    return ok


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("-"):
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    editor = HERE.parent / "out" / "deploy" / "Editor" / "HorizonEditor"
    for i, a in enumerate(args):
        if a == "--editor":
            editor = pathlib.Path(args[i + 1]).resolve()
    if not editor.exists():
        log("no editor at %s" % editor)
        return 2
    os.environ.pop("HE_GAME_SDK_DIR", None)
    os.environ.pop("HE_SIMULATE_MISSING_TOOLCHAIN", None)
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)
    R.setup(outdir, PROJECT_SRC)

    ok = {}
    ed = R.Editor(outdir, "order", editor, 1800)
    try:
        ed.wait_endpoint()
        c = Client("A", ed.endpoint)
        time.sleep(3.0)   # the toolchain probe answers in the background
        for tag in ("plain", "hc", "enc"):
            if tag == "hc":
                author_horizon_code(c)
            st, timeline, midrun, lines = run_export(c, outdir, tag, tag != "plain", tag == "enc")
            for snap in timeline:
                log("  [%s %6.2fs] %s  %s" % (tag, snap["t"],
                    "  ".join("%s=%s" % (n, s) for n, s in snap["steps"]), snap["activity"][:60]))
            ok[tag] = check_order(tag, st, timeline, lines)
            log("── %s: %s (mid-run snapshot: %s)" % (tag, "as expected" if ok[tag] else "NOT as expected",
                                                    "yes" if midrun else "none"))
        c.close()
    finally:
        ed.stop()

    log("RESULT " + " ".join("%s=%s" % (k, v) for k, v in ok.items()))
    return 0 if ok and all(ok.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
