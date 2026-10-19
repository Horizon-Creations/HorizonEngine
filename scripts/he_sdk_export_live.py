#!/usr/bin/env python3
"""he_sdk_export_live — every export compiles its game from the SDK (topic 222).

Starts the deployed HorizonEditor (out/deploy/Editor) hidden, with a private
HOME/config and a copy of the tutorial project, and exports through its Remote
Control bridge (`project_package`, target Host). Since step 3 there is no
"Compile game from SDK" option and no prebuilt game to fall back on:

  run "sdk"       the toolchain as found: Development must compile the game
                  from the dev lib set ("Game compiled from the SDK (dev)").
                  Shipping must compile from the release set when this editor
                  has one, and otherwise be REFUSED before it starts, naming
                  the 'release' lib set (never a prebuilt game).
  run "nocc"      the same editor with HE_SIMULATE_MISSING_TOOLCHAIN=compiler:
                  project_package must be refused with the compiler named, and
                  no export folder may appear.

HE_GAME_SDK_DIR is removed from the environment, so the editor has to find the
SDK the way the dialog does: findGameSdk(SDL_GetBasePath(), profile), i.e.
<editor>/EngineSDK or <editor>/../EngineSDK. Build logs go to
OUTDIR/build-<profile>.log, the exports to OUTDIR/export/<tag>.

  usage: he_sdk_export_live.py OUTDIR [--editor PATH]

Exit code 0 when every expectation above held.
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


def package(c, outdir, tag, profile):
    """project_package for one profile: (refusal text or None, final status)."""
    args = {"outputDir": str(outdir / "export" / tag), "incremental": False,
            "textureFormat": "None", "compress": False, "encrypt": False,
            "compileHorizonCode": False, "profile": profile}
    t0 = time.time()
    while True:
        try:
            c.call("project_package", args)
            break
        except RuntimeError as e:
            # The toolchain probe (a cmake configure) may still be running.
            if "still checking" in str(e) and time.time() - t0 < 120:
                time.sleep(1.0)
                continue
            log("  %s: refused at start: %s" % (tag, e))
            return str(e), None
    t0 = time.time()
    while time.time() - t0 < 600:
        _, st, _ = c.call("project_build_status", {"log": False})
        if st.get("finished") and not st.get("running"):
            log("  %s: finished after %.1fs success=%s message=%s"
                % (tag, time.time() - t0, st.get("success"), st.get("message")))
            return None, st
        time.sleep(0.5)
    raise RuntimeError("%s: export did not finish in 600s" % tag)


def has_release_set(editor):
    for d in (editor.parent / "EngineSDK", editor.parent.parent / "EngineSDK"):
        if (d / "lib" / "cmake" / "HorizonEngine" / "HorizonEngineTargets-release.cmake").exists():
            return True
    return False


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
    release = has_release_set(editor)
    log("release lib set beside the editor: %s" % release)

    ok = {}
    ed = R.Editor(outdir, "sdk", editor, 600)
    try:
        ed.wait_endpoint()
        c = Client("A", ed.endpoint)
        time.sleep(3.0)   # the toolchain probe answers in the background
        for profile, libset in PROFILES:
            refused, st = package(c, outdir, profile, profile)
            lines = []
            if st is not None:
                _, full, _ = c.call("project_build_status", {"log": True, "maxLines": 2000})
                lines = [l.get("text", "") if isinstance(l, dict) else str(l)
                         for l in full.get("log", [])]
                (outdir / ("build-%s.log" % profile)).write_text("\n".join(lines))
                for l in lines:
                    if "SDK" in l or "prebuilt" in l or "Removed" in l:
                        log("   " + l[:300])
            compiled = any(("Game compiled from the SDK (%s)" % libset) in l for l in lines)
            if libset == "dev" or release:
                ok[profile] = bool(st and st.get("success") and compiled)
            else:
                # The refusal arrives as the repr of the tool's content list, so
                # its quotes are escaped: match the words, not the quoting.
                ok[profile] = (refused is not None and "release" in refused
                               and "lib set" in refused and st is None)
            log("── %s: %s" % (profile, "as expected" if ok[profile] else "NOT as expected"))
        c.close()
    finally:
        ed.stop()

    os.environ["HE_SIMULATE_MISSING_TOOLCHAIN"] = "compiler"
    ed = R.Editor(outdir, "nocc", editor, 600)
    try:
        ed.wait_endpoint()
        c = Client("B", ed.endpoint)
        time.sleep(3.0)
        refused, st = package(c, outdir, "nocc", "Development")
        ok["no compiler"] = (refused is not None and "compiler" in refused and st is None
                             and not (outdir / "export" / "nocc").exists())
        log("── no compiler: %s" % ("as expected" if ok["no compiler"] else "NOT as expected"))
        c.close()
    finally:
        ed.stop()
        os.environ.pop("HE_SIMULATE_MISSING_TOOLCHAIN", None)

    log("RESULT " + " ".join("%s=%s" % (k.replace(" ", "_"), v) for k, v in ok.items()))
    return 0 if all(ok.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
