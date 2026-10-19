#!/usr/bin/env python3
"""he_sdk_export_live — "Compile game from SDK" against a live editor (topic 222).

Starts the deployed HorizonEditor (out/deploy/Editor) hidden, with a private
HOME/config and a copy of the tutorial project, and exports twice through its
Remote Control bridge (`project_package`, target Host, compileGameFromSdk on):
once as Development (dev lib set) and once as Shipping (release lib set).
HE_GAME_SDK_DIR is removed from the environment, so the editor has to find the
SDK the way the dialog does: findGameSdk(SDL_GetBasePath(), profile), i.e.
<editor>/EngineSDK or <editor>/../EngineSDK, where the editor build's HorizonSDK
dependency installs it.

The witness is the export's build log: "Game compiled from the SDK (<set>)"
versus "Compile game from SDK: <why> — the prebuilt game ships". Both logs are
written to OUTDIR/build-<profile>.log, the exports to OUTDIR/export/<profile>.

  usage: he_sdk_export_live.py OUTDIR [--editor PATH] [--require dev|release|both]

Exit code 0 when every lib set named by --require (default dev) compiled the
game; a set that is not required is only reported.
"""
import os
import pathlib
import shutil
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import he_rewards_live as R  # noqa: E402  — Editor, setup, export
from he_mcp_multiclient import Client, PROJECT_SRC, log  # noqa: E402

PROFILES = (("Development", "dev"), ("Shipping", "release"))


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("-"):
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    editor = HERE.parent / "out" / "deploy" / "Editor" / "HorizonEditor"
    require = "dev"
    for i, a in enumerate(args):
        if a == "--editor":
            editor = pathlib.Path(args[i + 1]).resolve()
        if a == "--require":
            require = args[i + 1]
    required = {"dev", "release"} if require == "both" else {require}
    if not editor.exists():
        log("no editor at %s" % editor)
        return 2
    os.environ.pop("HE_GAME_SDK_DIR", None)
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)
    R.setup(outdir, PROJECT_SRC)

    compiled = {}
    ed = R.Editor(outdir, "sdk", editor, 600)
    try:
        ed.wait_endpoint()
        c = Client("A", ed.endpoint)
        time.sleep(3.0)
        for profile, libset in PROFILES:
            st = R.export(c, outdir, profile, {"profile": profile, "compileGameFromSdk": True})
            _, full, _ = c.call("project_build_status", {"log": True, "maxLines": 2000})
            lines = [l.get("text", "") if isinstance(l, dict) else str(l)
                     for l in full.get("log", [])]
            (outdir / ("build-%s.log" % profile)).write_text("\n".join(lines))
            log("── %s: success=%s message=%s" % (profile, st and st.get("success"),
                                                  st and st.get("message")))
            for l in lines:
                if "SDK" in l or "prebuilt" in l:
                    log("   " + l[:300])
            compiled[libset] = any(("Game compiled from the SDK (%s)" % libset) in l
                                   for l in lines)
        c.close()
    finally:
        ed.stop()

    log("RESULT " + " ".join("%s_compiled=%s" % (k, compiled.get(k)) for _, k in PROFILES))
    return 0 if all(compiled.get(k) for k in required) else 1


if __name__ == "__main__":
    sys.exit(main())
