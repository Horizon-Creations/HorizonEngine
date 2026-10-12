#!/usr/bin/env python3
"""The self-update live leg for the CI matrix (Thema 186 Schritt 4).

test_selfupdate.cpp's "live round trip" and "wrong password" cases
(tests/test_selfupdate.cpp) are skipped unless HE_SELFUPDATE_LIVE_TEST=1 and a
real HE_SELFUPDATE_HOST/USER/PASSWORD are set — on purpose, so an ordinary
`ctest` run needs no network and no secret. This script is what supplies
those: it stands up scripts/selfupdate_test_sftp_server.py as a real,
throwaway, read-only SFTP account on 127.0.0.1 with a freshly generated
password and a one-build manifest for whichever platform it runs on, points
HE_SELFUPDATE_* at it, and runs just those two ctest cases. No credential
leaves this process — the password is generated here, handed to the server
and to he_tests through the environment, and never written to a file or
printed. Needs `pip install paramiko` (the server's own dependency).

A doctest case skipped is a ctest PASS, not a failure — so a wiring mistake
that never actually sets HE_SELFUPDATE_LIVE_TEST on the child process would
report green while proving nothing. This script therefore also parses
doctest's own summary line out of the captured output and insists on
"0 skipped": the two live cases, and only those two, are skip-gated in that
test file.

    python3 scripts/selfupdate_live_ci_check.py --build-dir build
"""
import argparse
import hashlib
import os
import queue
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading


def current_platform_key():
    """Mirrors HE::Su::currentPlatformKey() (SelfUpdateCheck.cpp)."""
    import platform

    system = platform.system()
    if system == "Windows":
        os_name = "windows"
    elif system == "Darwin":
        os_name = "macos"
    elif system == "Linux":
        os_name = "linux"
    else:
        os_name = "unknown"

    machine = platform.machine().lower()
    if machine in ("x86_64", "amd64"):
        arch = "x64"
    elif machine in ("arm64", "aarch64"):
        arch = "arm64"
    else:
        arch = "unknown"
    return "%s-%s" % (os_name, arch)


def write_fixture(root, platform_key, artifact_size):
    """<root>/packages/manifest.json + the one artifact it names."""
    packages = os.path.join(root, "packages")
    os.makedirs(packages, exist_ok=True)

    payload = secrets.token_bytes(artifact_size)
    artifact_path = os.path.join(packages, "payload.bin")
    with open(artifact_path, "wb") as f:
        f.write(payload)
    digest = hashlib.sha256(payload).hexdigest()

    manifest = (
        '{"schema":1,"version":"9.9.9","codename":"CI","artifacts":['
        '{"platform":"%s","path":"payload.bin","sha256":"%s","size":%d}]}'
    ) % (platform_key, digest, len(payload))
    with open(os.path.join(packages, "manifest.json"), "w", encoding="utf-8") as f:
        f.write(manifest)


def wait_for_ready(proc, timeout_s):
    """Reads the server's single "READY port=<n>" stdout line, or raises.

    proc.stdout.readline() has no timeout of its own, so it is read on a
    daemon thread and joined with a real deadline here — a server that never
    prints (hung bind, crash before the print) must not hang this process:
    the caller kills `proc` on a RuntimeError, which unblocks the thread via
    EOF even though nothing joins it.
    """
    q = queue.Queue(maxsize=1)
    threading.Thread(target=lambda: q.put(proc.stdout.readline()), daemon=True).start()
    try:
        line = q.get(timeout=timeout_s)
    except queue.Empty:
        raise RuntimeError("the test SFTP server printed no READY line within %ss" % timeout_s)
    line = line.strip()
    if not line.startswith("READY port="):
        code = proc.poll()
        extra = " (process exited, code %s)" % code if code is not None else ""
        raise RuntimeError("did not see a READY line from the test SFTP server, got: %r%s" % (line, extra))
    return int(line[len("READY port="):])


_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
_SUMMARY_RE = re.compile(
    r"test cases:\s*\d+\s*\|\s*\d+\s*passed\s*\|\s*\d+\s*failed\s*\|\s*(\d+)\s*skipped"
)


def check_nothing_was_skipped(ctest_output):
    """Raises unless doctest's own summary says '0 skipped'.

    ctest returning 0 is not enough: a doctest case gated by doctest::skip(...)
    that never actually ran (e.g. the live-test env didn't reach the child
    process) is itself a PASS as far as ctest is concerned. test_selfupdate.cpp
    has exactly two skip-gated cases (the live ones), and only HE_SELFUPDATE_
    LIVE_TEST unset should ever cause a skip — count >0 here means that
    happened despite this script setting it.
    """
    text = _ANSI_RE.sub("", ctest_output)
    match = _SUMMARY_RE.search(text)
    if not match:
        raise RuntimeError("could not find doctest's 'test cases: ... skipped' summary in the ctest output")
    skipped = int(match.group(1))
    if skipped != 0:
        raise RuntimeError(
            "%d doctest case(s) were skipped — the live round trip did not actually run "
            "(HE_SELFUPDATE_LIVE_TEST likely did not reach he_tests)" % skipped
        )


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build-dir", default="build", help="the ctest build directory")
    ap.add_argument("--artifact-size", type=int, default=3 * 1024 * 1024,
                    help="bytes of the fixture 'build' the manifest offers (default 3 MiB)")
    ap.add_argument("--server-timeout", type=float, default=20.0,
                    help="seconds to wait for the test SFTP server's READY line")
    ap.add_argument("--ctest-timeout", type=float, default=300.0,
                    help="seconds to wait for the live ctest cases")
    args = ap.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    server_script = os.path.join(script_dir, "selfupdate_test_sftp_server.py")

    platform_key = current_platform_key()
    user = "editor-ro"
    password = secrets.token_urlsafe(24)

    root = tempfile.mkdtemp(prefix="he_selfupdate_ci_")
    server_proc = None
    try:
        write_fixture(root, platform_key, args.artifact_size)
        print("selfupdate_live_ci_check: fixture at %s for platform '%s'" % (root, platform_key))

        env = os.environ.copy()
        env["HE_SELFUPDATE_PASSWORD"] = password
        # stderr is inherited (not piped): the server logs every file it serves
        # for the duration of the live cases, and nothing here would ever drain
        # a second pipe once wait_for_ready stops reading stdout after one line.
        server_proc = subprocess.Popen(
            [sys.executable, server_script, "--root", root, "--user", user, "--port", "0"],
            stdout=subprocess.PIPE, text=True, env=env,
        )
        port = wait_for_ready(server_proc, args.server_timeout)
        print("selfupdate_live_ci_check: test SFTP server ready on 127.0.0.1:%d" % port)

        env["HE_SELFUPDATE_LIVE_TEST"] = "1"
        env["HE_SELFUPDATE_HOST"] = "127.0.0.1"
        env["HE_SELFUPDATE_PORT"] = str(port)
        env["HE_SELFUPDATE_USER"] = user
        # HE_SELFUPDATE_PASSWORD is already in env, same value the server was given.

        result = subprocess.run(
            ["ctest", "--test-dir", args.build_dir, "-R", "^test_selfupdate$", "-V", "--no-tests=error"],
            env=env, timeout=args.ctest_timeout,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        print(result.stdout)
        if result.returncode != 0:
            return result.returncode
        check_nothing_was_skipped(result.stdout)
        print("selfupdate_live_ci_check: live round trip ran for real (0 skipped)")
        return 0
    finally:
        if server_proc is not None and server_proc.poll() is None:
            server_proc.terminate()
            try:
                server_proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server_proc.kill()
                server_proc.wait(timeout=10)
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as exc:
        print("::error::selfupdate_live_ci_check: %s" % exc)
        sys.exit(1)
