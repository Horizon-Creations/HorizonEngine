#!/usr/bin/env python3
"""he_quality_export_live - export a project with automatic quality on, ready for a governor run.

Starts the deployed HorizonEditor hidden (private config, copy of the project), turns the project's
`autoQuality` block on (target 1000 fps: the governor can only see a miss on a fast machine when the
target is above what the scene reaches) and exports it through the Remote Control bridge
(`project_package`, Development profile, target Host), the same way Build > Export Project does.
The game is compiled from the engine SDK, so this needs the editor's SDK dev lib set and a C++ toolchain.

  usage: he_quality_export_live.py OUTDIR --project DIR [--editor PATH] [--no-auto-quality]

--no-auto-quality exports the same project with the governor off: the reference game for a before/after frame.

The export lands in OUTDIR/export/dev. Run HorizonGame.exe from there (see scripts/he_quality_governor_run.ps1).
Exit code 0 when the export succeeded.
"""
import json
import os
import pathlib
import shutil
import sys
import time

if not sys.flags.utf8_mode:
    # The build log carries arrows and the like; Windows' default encoding cannot write them.
    os.execv(sys.executable, [sys.executable, "-X", "utf8"] + sys.argv)

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import he_rewards_live as R  # noqa: E402  - Editor, setup
from he_mcp_multiclient import Client, log  # noqa: E402
import he_export_step_order_live as X  # noqa: E402  - run_export


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("-"):
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    editor = HERE.parent / "out" / "deploy" / "Editor" / ("HorizonEditor.exe" if sys.platform == "win32" else "HorizonEditor")
    project = None
    auto = "--no-auto-quality" not in args
    for i, a in enumerate(args):
        if a == "--editor":
            editor = pathlib.Path(args[i + 1]).resolve()
        if a == "--project":
            project = pathlib.Path(args[i + 1]).resolve()
    if project is None or not project.exists():
        log("--project DIR is required")
        return 2
    if not editor.exists():
        log("no editor at %s" % editor)
        return 2
    os.environ.pop("HE_GAME_SDK_DIR", None)
    os.environ.pop("HE_SIMULATE_MISSING_TOOLCHAIN", None)
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)
    R.setup(outdir, project)

    ps = outdir / "project" / "Config" / "ProjectSettings.json"
    settings = json.loads(ps.read_text(encoding="utf-8")) if ps.exists() else {}
    settings["autoQuality"] = {"enabled": auto, "targetFps": 1000.0, "minLevel": 0, "maxLevel": 2}
    ps.parent.mkdir(parents=True, exist_ok=True)
    ps.write_text(json.dumps(settings, indent=1), encoding="utf-8")
    log("project settings: autoQuality %s, target 1000 fps" % ("on" if auto else "off"))

    ok = False
    ed = R.Editor(outdir, "export", editor, 1800)
    try:
        ed.wait_endpoint()
        c = Client("A", ed.endpoint)
        time.sleep(3.0)   # the toolchain probe answers in the background
        st, timeline, midrun, lines = X.run_export(c, outdir, "dev", False)
        for snap in timeline[-3:]:
            log("  [%6.2fs] %s  %s" % (snap["t"], "  ".join("%s=%s" % (n, s) for n, s in snap["steps"]),
                                       snap["activity"][:60]))
        ok = bool(st.get("success"))
        log("export %s (%s)" % ("succeeded" if ok else "FAILED", st.get("message")))
        c.close()
    finally:
        ed.stop()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
