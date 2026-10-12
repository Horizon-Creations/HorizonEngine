#!/usr/bin/env python3
"""selfupdate_editor_e2e — the editor's self-update, end to end, with unsaved work.

Thema 186 Schritt 6. Proves on a real editor process what the unit tests can
only prove in pieces: Help > Check for Updates > Download > Restart to Update,
started from an editor that has an OPEN PROJECT WITH AN UNSAVED CHANGE, goes
through the same Save / Don't Save / Cancel prompt as Quit, and

    --mode cancel   changes nothing: the editor keeps running, the change is
                    still unsaved in it, the install is untouched;
    --mode save     writes the change, quits, HorizonUpdater swaps the install,
                    and the NEW build comes up with the same project open and
                    the change in it;
    --mode discard  quits without writing (the user's choice), the new build
                    comes up with the project as it was on disk.

Everything runs against throwaway copies under OUT: the install (a copy of the
given build), a private HOME (config, staging, MCP endpoint), a copy of the
project, and a local read-only SFTP account (selfupdate_test_sftp_server.py,
needs paramiko) serving a "9.9.9" build that is the same editor plus a marker
file — so the swap is visible on disk. The editor runs hidden
(HE_HIDDEN_WINDOW=1); nobody clicks anything. The scene is made dirty over the
editor's Remote Control bridge (entity_create, the same path as any MCP
client), and the dialog's buttons and the prompt are pressed by the editor's
own acceptance driver (HE_SELFUPDATE_E2E, SelfUpdateDialog.h).

    scripts/selfupdate_editor_e2e.py --install-src out/dmg_staging/HorizonEditor.app \\
        --mode save --out /tmp/su_e2e_save [--server-python /path/to/venv/python]

--install-src is the build that plays "installed": the .app bundle on macOS,
the flat deploy directory (HorizonEditor[.exe] + libraries + HorizonUpdater)
on Linux and Windows. Exit code 0 = every expectation of the mode held;
OUT/result.json lists them.

Verified on macOS (arm64). The Linux/Windows branches (tar.gz / zip artifact,
flat install) follow SelfUpdateInstall.cpp but have not been run yet.
"""

import argparse
import hashlib
import json
import os
import pathlib
import platform
import queue
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import threading
import time
import zipfile

SCRIPTS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))
import he_mcp  # noqa: E402

MARKER_ENTITY = "SelfUpdateE2E_UnsavedMarker"
NEW_BUILD_MARKER = "selfupdate-e2e-new-build.txt"
DEFAULT_PROJECT = pathlib.Path.home() / "Documents" / "HorizonEngine" / "HorizonTutorial"


def log(msg):
    print("[e2e] " + msg, flush=True)


def platform_key():
    """Mirrors HE::Su::currentPlatformKey()."""
    system = platform.system()
    os_name = {"Windows": "windows", "Darwin": "macos", "Linux": "linux"}.get(system, "unknown")
    machine = platform.machine().lower()
    arch = "x64" if machine in ("x86_64", "amd64") else "arm64" if machine in ("arm64", "aarch64") else "unknown"
    return "%s-%s" % (os_name, arch)


def user_data_dir(home):
    """Mirrors GlobalState::userDataDir() for a private HOME/APPDATA."""
    if sys.platform == "darwin":
        return home / "Library" / "Application Support" / "HorizonEngine"
    if os.name == "nt":
        return home / "AppData" / "Roaming" / "HorizonEngine"
    return home / ".config" / "HorizonEngine"


def main_exe(install_root):
    if sys.platform == "darwin":
        return install_root / "Contents" / "MacOS" / "HorizonEditor"
    return install_root / ("HorizonEditor.exe" if os.name == "nt" else "HorizonEditor")


def marker_path(install_root):
    if sys.platform == "darwin":
        return install_root / "Contents" / "Resources" / NEW_BUILD_MARKER
    return install_root / NEW_BUILD_MARKER


def copy_tree(src, dst):
    if sys.platform == "darwin":
        subprocess.run(["/usr/bin/ditto", str(src), str(dst)], check=True)
    else:
        shutil.copytree(src, dst, symlinks=True)


def build_artifact(install_src, work, key):
    """The '9.9.9' build: the same editor plus a marker file, packed the way
    SelfUpdateInstall.cpp unpacks it for this platform."""
    stage = work / "newbuild"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    root = stage / install_src.name
    copy_tree(install_src, root)
    marker_path(root).write_text("9.9.9 (selfupdate_editor_e2e)\n")
    if sys.platform == "darwin":
        # Adding a file breaks the bundle's seal; sign it again, ad hoc, as the
        # packager does.
        subprocess.run(["codesign", "--force", "--deep", "-s", "-", str(root)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out = work / ("HorizonEditor-%s.dmg" % key)
        subprocess.run(["hdiutil", "create", "-quiet", "-volname", "HorizonEditor", "-srcfolder", str(stage),
                        "-format", "UDZO", "-ov", str(out)], check=True)
    elif os.name == "nt":
        out = work / ("HorizonEditor-%s.zip" % key)
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
            for p in root.rglob("*"):
                z.write(p, pathlib.Path("Editor") / p.relative_to(root))
    else:
        out = work / ("HorizonEditor-%s.tar.gz" % key)
        with tarfile.open(out, "w:gz") as t:
            t.add(root, arcname="Editor")
    return out


def write_server(server_root, artifact, key):
    rel = "releases/9.9.9/" + artifact.name
    dst = server_root / "packages" / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(artifact, dst)
    data = dst.read_bytes()
    manifest = {"schema": 1, "version": "9.9.9", "codename": "E2E",
                "notes": "selfupdate_editor_e2e fixture build",
                "artifacts": [{"platform": key, "path": rel,
                               "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}]}
    (server_root / "packages" / "manifest.json").write_text(json.dumps(manifest))


def start_server(python, root, env):
    proc = subprocess.Popen([python, str(SCRIPTS / "selfupdate_test_sftp_server.py"), "--root", str(root),
                             "--user", "editor-e2e", "--port", "0"],
                            stdout=subprocess.PIPE, text=True, env=env)
    q = queue.Queue(maxsize=1)
    threading.Thread(target=lambda: q.put(proc.stdout.readline()), daemon=True).start()
    line = q.get(timeout=30).strip()
    if not line.startswith("READY port="):
        proc.kill()
        raise RuntimeError("SFTP test server did not come up: %r" % line)
    return proc, int(line[len("READY port="):])


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(cond, timeout, what, poll=0.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        v = cond()
        if v:
            return v
        time.sleep(poll)
    raise RuntimeError("timed out after %ds waiting for %s" % (timeout, what))


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


class Bridge:
    """An MCP client of whichever editor currently owns the endpoint file."""

    def __init__(self, endpoint):
        self.endpoint = endpoint

    def info(self):
        try:
            return he_mcp.read_endpoint(str(self.endpoint))
        except Exception:
            return None

    def call(self, tool, args=None):
        b = he_mcp.Bridge(str(self.endpoint))
        b.ensure()
        reply = b.request("tools/call", {"name": tool, "arguments": args or {}})
        if "error" in reply:
            raise RuntimeError("%s -> rpc error %s" % (tool, reply["error"]))
        res = reply["result"]
        if res.get("isError"):
            raise RuntimeError("%s -> tool error %s" % (tool, res.get("content")))
        return res.get("structuredContent") or {}


def names_in_scene(bridge):
    sc = bridge.call("entity_list", {})
    out = []

    def walk(items):
        for e in items or []:
            out.append(e.get("name", ""))
            walk(e.get("children"))
    walk(sc.get("entities"))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install-src", required=True, type=pathlib.Path)
    ap.add_argument("--mode", required=True, choices=["save", "cancel", "discard"])
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--project", type=pathlib.Path, default=DEFAULT_PROJECT)
    ap.add_argument("--server-python", default=sys.executable, help="a Python with paramiko")
    ap.add_argument("--timeout", type=int, default=240, help="seconds for each wait")
    args = ap.parse_args()

    out = args.out.resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    key = platform_key()
    checks = []

    def expect(ok, what):
        checks.append({"ok": bool(ok), "what": what})
        log(("PASS " if ok else "FAIL ") + what)

    # ── Fixture ──────────────────────────────────────────────────────────────
    install = out / "install" / args.install_src.name
    install.parent.mkdir(parents=True)
    copy_tree(args.install_src.resolve(), install)
    artifact = build_artifact(args.install_src.resolve(), out, key)
    write_server(out / "server", artifact, key)
    project = out / "project"
    shutil.copytree(args.project, project, ignore=shutil.ignore_patterns("Saved", ".git"))
    heproj = next(project.glob("*.heproj"))
    home = out / "home"
    userdata = user_data_dir(home)
    userdata.mkdir(parents=True)
    cfg = {"LastProjectPath": str(heproj), "KnownProjects": [str(heproj)], "CustomConfig": []}
    if sys.platform == "darwin":
        cfg["RHI"] = 4   # Metal; the ImGui backend follows the config, not HE_DUMP_RHI
    (userdata / "config.json").write_text(json.dumps(cfg))
    log("fixture under %s (artifact %s, %d bytes)" % (out, artifact.name, artifact.stat().st_size))

    password = secrets.token_urlsafe(18)
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("HE_SELFUPDATE_"):
            del env[k]
    env["HE_SELFUPDATE_PASSWORD"] = password
    server, sport = start_server(args.server_python, out / "server", env)
    log("SFTP test account on 127.0.0.1:%d" % sport)

    mcp_port = free_port()
    trigger = out / "e2e.trigger"
    env.update({
        "HOME": str(home), "APPDATA": str(home / "AppData" / "Roaming"),
        "HE_MCP": "1", "HE_MCP_PORT": str(mcp_port),
        "HE_HIDDEN_WINDOW": "1", "HE_COLLAB_OFFLINE": "1",
        "HE_SELFUPDATE_HOST": "127.0.0.1", "HE_SELFUPDATE_PORT": str(sport),
        "HE_SELFUPDATE_USER": "editor-e2e",
        "HE_SELFUPDATE_E2E": args.mode, "HE_SELFUPDATE_E2E_TRIGGER": str(trigger),
    })
    if sys.platform == "darwin":
        env.setdefault("HE_DUMP_RHI", "Metal")
    endpoint = userdata / "mcp-endpoint.json"
    bridge = Bridge(endpoint)
    old_log = open(out / "editor-old.log", "wb")
    editor = subprocess.Popen([str(main_exe(install)), str(heproj)], env=env, stdout=old_log,
                              stderr=subprocess.STDOUT, cwd=str(out))
    log("editor (0.x, installed) started, pid %d" % editor.pid)
    new_pid = None
    try:
        info = wait_for(lambda: (lambda i: i if i and i.get("pid") == editor.pid else None)(bridge.info()),
                        args.timeout, "the editor's MCP endpoint")
        state = wait_for(lambda: (lambda s: s if s.get("sceneOpen") else None)(bridge.call("scene_info")),
                         args.timeout, "the project to open")
        scene_path = pathlib.Path(state["scene"])
        if not scene_path.is_absolute():
            scene_path = project / "Content" / scene_path
        log("project open: %s, scene %s" % (state.get("project"), scene_path))
        before = scene_path.read_text(errors="replace") if scene_path.exists() else ""
        expect(MARKER_ENTITY not in before, "the scene on disk has no marker entity before the run")

        bridge.call("entity_create", {"name": MARKER_ENTITY, "position": [0.0, 2.0, 0.0]})
        state = bridge.call("scene_info")
        expect(state.get("dirty") is True, "scene is dirty after the unsaved change (scene_info.dirty)")

        trigger.write_text("go")
        log("trigger written: Check for Updates -> Download -> Restart to Update -> prompt (%s)" % args.mode)
        text = lambda: (out / "editor-old.log").read_text(errors="replace")

        if args.mode == "cancel":
            wait_for(lambda: "update cancelled at the unsaved-changes prompt" in text(), args.timeout,
                     "the prompt to be cancelled")
            time.sleep(3)
            t = text()
            expect("asking before the update" in t, "the unsaved-changes prompt was raised for the update")
            expect(editor.poll() is None, "the editor is still running after Cancel")
            expect("HorizonUpdater started" not in t, "HorizonUpdater was never started")
            state = bridge.call("scene_info")
            expect(state.get("dirty") is True, "the change is still unsaved in the running editor")
            expect(MARKER_ENTITY in names_in_scene(bridge), "the unsaved entity is still in the open scene")
            expect(not marker_path(install).exists(), "the install was not touched")
            after = scene_path.read_text(errors="replace") if scene_path.exists() else ""
            expect(after == before, "the scene file on disk is unchanged")
            return 0 if all(c["ok"] for c in checks) else 1

        # save / discard: the editor quits, HorizonUpdater swaps, the new build comes up.
        rc = wait_for(lambda: editor.poll() is not None or None, args.timeout, "the old editor to quit")
        old_log.flush()
        t = text()
        expect("asking before the update" in t, "the unsaved-changes prompt was raised for the update")
        answered = "Save" if args.mode == "save" else "Discard"
        expect("prompt answered with %s" % answered in t, "the prompt was answered with %s" % answered)
        expect("HorizonUpdater started" in t, "the editor started HorizonUpdater before quitting")
        log("old editor exited with %s" % editor.returncode)

        last = userdata / "SelfUpdate" / "last-apply.json"
        result = wait_for(lambda: json.loads(last.read_text()) if last.exists() else None, args.timeout,
                          "HorizonUpdater's last-apply.json")
        expect(result.get("status") == "applied", "HorizonUpdater reports the update applied (%s)" % result)
        expect(marker_path(install).exists(), "the install now holds the 9.9.9 build (marker file)")

        after = scene_path.read_text(errors="replace") if scene_path.exists() else ""
        if args.mode == "save":
            expect(MARKER_ENTITY in after, "Save wrote the unsaved change to the scene file before the update")
        else:
            expect(after == before, "Discard left the scene file as it was on disk (the user's choice)")

        info2 = wait_for(lambda: (lambda i: i if i and i.get("pid") not in (None, editor.pid) else None)(bridge.info()),
                         args.timeout, "the relaunched editor's MCP endpoint")
        new_pid = info2["pid"]
        exe_of_new = subprocess.run(["ps", "-o", "command=", "-p", str(new_pid)], capture_output=True,
                                    text=True).stdout.strip() if os.name != "nt" else ""
        log("relaunched editor pid %d: %s" % (new_pid, exe_of_new))
        if exe_of_new:
            expect(str(install) in exe_of_new, "the relaunched editor runs from the swapped install")
            expect(str(heproj) in exe_of_new, "the relaunched editor was started with the project file")
        state2 = wait_for(lambda: (lambda s: s if s.get("sceneOpen") else None)(bridge.call("scene_info")),
                          args.timeout, "the relaunched editor to open the project")
        expect(state2.get("project") == state.get("project") if state.get("project") else True,
               "the relaunched editor has the same project open (%s)" % state2.get("project"))
        expect(state2.get("dirty") is False, "the relaunched editor starts clean")
        has_marker = MARKER_ENTITY in names_in_scene(bridge)
        if args.mode == "save":
            expect(has_marker, "the saved change is in the scene the relaunched editor shows")
        else:
            expect(not has_marker, "the discarded change is not in the relaunched editor's scene")
        return 0 if all(c["ok"] for c in checks) else 1
    except Exception as e:  # report, then clean up
        expect(False, "aborted: %s" % e)
        return 1
    finally:
        for pid in filter(None, [editor.pid if editor.poll() is None else None, new_pid]):
            if pid_alive(pid):
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
        server.kill()
        old_log.close()
        newlog = userdata / "HorizonEngine.log"
        if newlog.exists():
            shutil.copy2(newlog, out / "editor-userdata.log")
        (out / "result.json").write_text(json.dumps({"mode": args.mode, "platform": key, "checks": checks},
                                                    indent=2))
        log("%d/%d checks passed -> %s" % (sum(c["ok"] for c in checks), len(checks), out / "result.json"))


if __name__ == "__main__":
    sys.exit(main())
