#!/usr/bin/env python3
"""he_team_token_restart — a real editor stays signed in to its team across restarts.

tests/test_net_team.cpp proves the session token (topic 201) with TeamControllers in
one process. This is the run it cannot be: ONE private editor profile, started five
times in a row, against a real team server, with a raw socket as "another device"
of the same team. Nothing is typed twice: the password goes in once per sign-in
through team_sign_in (the TeamController::signIn the Sign In button calls), and the
starts after it get no password at all.

The profile's config.json carries CollabRelayAddress, TeamName and TeamMember from
the start, as the Preferences > Team fields would after the person typed them
(team_sign_in does not write them). HE_CONFIG_DIR points at the profile, so
team-session.txt lands beside that config.json and never in the human's.

What it checks:
  1. start 1: password sign-in → team-session.txt (0600, no password in it or in
     config.json), the server issued a token; quit (SIGTERM, clean) keeps both files;
  2. start 2: signed in on its own, with the token (editor and server log say so);
     Sign Out → the file is gone and the server revoked the token;
  3. start 3 (negative control): no file → stays signed out; sign in with the password;
  4. start 4: signed in with the token; "another device" of the same member signs
     out everywhere (REVOKE_ALL_TOKENS) → the live editor fails with "saved sign-in is
     no longer valid", the file is gone; sign in with the password again; quit;
  5. while the editor is off the admin removes the member; start 5: the refused token
     is forgotten at start (failed, file gone, the notice text), not retried.

Usage:
    scripts/he_team_token_restart.py OUTDIR --server HOST:PORT [--server-log PATH]
                                     [--editor PATH] [--timeout S]

--server-log is the collab_server's log (local server only); with it the server's
side of every step is checked too. Each run leaves its team tok-<time> on the server.
Prints `RESULT: PASS|FAIL` last; the editor logs and a transcript go into OUTDIR.
"""

import json
import os
import pathlib
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import he_mcp_multiclient as mc  # noqa: E402
from he_collab_two_editors import free_port, wait_for  # noqa: E402

log = mc.log

TIMEOUT = 30.0
ANNA_PW = "anna-password-201"
BOB_PW = "bob-password-201"


# ── "Another device": the team protocol over a raw socket ────────────────────
# Wire format: docs/team-login-persist-2026-10-14.md and the CollabSession README.

MAGIC, VERSION = b"HERL", 1
T_STATUS, T_TEAM_LOGIN, T_TEAM_REQUEST, T_TEAM_REPLY, T_TEAM_EVENT = 2, 6, 7, 8, 9
OP_LOGIN, OP_CREATE = 1, 2
ADD_MEMBER, REMOVE_MEMBER, REVOKE_ALL_TOKENS = 2, 3, 15
LOGGED_IN = 4


def s8(text):
    b = text.encode()
    return bytes([len(b)]) + b


def frame(payload):
    return struct.pack(">I", len(payload)) + payload


class Device:
    """One team connection that logs in with the password, as a second machine would."""

    def __init__(self, server, team, member, password, create=False):
        host, _, port = server.rpartition(":")
        self.sock = socket.create_connection((host, int(port)), timeout=10)
        self.sock.sendall(frame(MAGIC + bytes([VERSION, T_TEAM_LOGIN, OP_CREATE if create else OP_LOGIN]) +
                                s8(team) + s8(member) + s8(password)))
        payload = self.next_frame()
        if payload[5] != T_STATUS or payload[6] != LOGGED_IN:
            raise RuntimeError("login as %s refused: %r" % (member, payload))
        self.next_id = 1

    def recv_exact(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise RuntimeError("team server closed the connection")
            data += chunk
        return data

    def next_frame(self):
        while True:
            (length,) = struct.unpack(">I", self.recv_exact(4))
            payload = self.recv_exact(length)
            if payload[5] != T_TEAM_EVENT:        # events come at any time; skip them
                return payload

    def request(self, op, body=b""):
        rid, self.next_id = self.next_id, self.next_id + 1
        self.sock.sendall(frame(MAGIC + bytes([VERSION, T_TEAM_REQUEST]) + struct.pack(">IB", rid, op) + body))
        while True:
            payload = self.next_frame()
            if payload[5] != T_TEAM_REPLY:
                raise RuntimeError("expected a team reply, got %r" % payload)
            got, result = struct.unpack(">IB", payload[6:11])
            if got == rid:
                n = payload[11]
                return result, payload[12:12 + n].decode()

    def close(self):
        self.sock.close()


# ── One editor profile, started again and again ──────────────────────────────

class Profile:
    """A private HOME + config dir; start() launches the build-tree editor on it."""

    def __init__(self, outdir, editor_path, timeout, server, team, member):
        self.outdir = pathlib.Path(outdir)
        self.home = self.outdir / "home"
        self.userdata = self.home / "Library" / "Application Support" / "HorizonEngine"
        self.userdata.mkdir(parents=True, exist_ok=True)
        self.config = self.userdata / "config.json"
        self.token = self.userdata / "team-session.txt"
        self.endpoint = self.userdata / "mcp-endpoint.json"
        self.editor_path = editor_path
        self.timeout = timeout
        self.runs = 0
        self.proc = None
        cfg = {"RHI": 4, "CustomConfig": [
            {"Key": "CollabRelayAddress", "Value": server},
            {"Key": "TeamName", "Value": team},
            {"Key": "TeamMember", "Value": member},
        ]}
        if mc.PROJECT_SRC.exists():
            project = self.outdir / "project"
            shutil.copytree(mc.PROJECT_SRC, project)
            heproj = next(project.glob("*.heproj"))
            cfg.update({"LastProjectPath": str(heproj), "KnownProjects": [str(heproj)]})
        self.config.write_text(json.dumps(cfg))

    def start(self):
        self.runs += 1
        if self.endpoint.exists():
            self.endpoint.unlink()     # wait_endpoint must not take the last run's file
        self.port = free_port()
        env = dict(os.environ)
        env.update({"HOME": str(self.home), "HE_CONFIG_DIR": str(self.userdata), "HE_MCP": "1",
                    "HE_MCP_PORT": str(self.port), "HE_MCP_COLLAB_CONTROL": "1", "HE_COLLAB_OFFLINE": "1",
                    "HE_HIDDEN_WINDOW": "1"})
        env.setdefault("HE_DUMP_RHI", "Metal")
        self.logpath = self.outdir / ("editor-%d.log" % self.runs)
        self.logfile = open(self.logpath, "wb")
        self.proc = subprocess.Popen([str(self.editor_path)], env=env, stdout=self.logfile,
                                     stderr=subprocess.STDOUT, cwd=str(self.editor_path.parent))
        log("start %d: pid %d" % (self.runs, self.proc.pid))
        deadline = time.time() + self.timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("editor exited early with rc %s" % self.proc.returncode)
            if self.endpoint.exists() and self.endpoint.stat().st_size > 0:
                try:
                    if mc.he_mcp.read_endpoint(str(self.endpoint))["port"] == self.port:
                        break
                except mc.he_mcp.ShimError:
                    pass
            time.sleep(0.5)
        else:
            raise RuntimeError("no endpoint file after %ds" % self.timeout)
        self.client = mc.Client("ed%d" % self.runs, self.endpoint)

    def call(self, tool, args=None):
        reply = self.client.bridge.request("tools/call", {"name": tool, "arguments": args or {}})
        if "error" in reply:
            raise RuntimeError("%s → rpc error %s" % (tool, reply["error"]))
        res = reply["result"]
        sc = res.get("structuredContent") or {}
        if res.get("isError"):
            raise RuntimeError("%s → %s: %s" % (tool, sc.get("code"), sc.get("message")))
        return sc

    def team(self):
        return self.call("team_status")

    def status_is(self, want):
        st = self.team()
        return st if st.get("status") == want else None

    def quit(self):
        """SIGTERM = the window's close: OnShutdown writes config.json and disconnects."""
        self.client.bridge.close()
        self.proc.send_signal(signal.SIGTERM)
        try:
            rc = self.proc.wait(30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = None
        self.logfile.close()
        log("start %d: quit, rc %s" % (self.runs, rc))
        return rc

    def log_text(self):
        return self.logpath.read_text(errors="replace")

    def config_value(self, key):
        for e in json.loads(self.config.read_text()).get("CustomConfig", []):
            if e.get("Key") == key:
                return e.get("Value")
        return None


def server_lines(path, needle):
    if not path:
        return None
    return [l for l in pathlib.Path(path).read_text(errors="replace").splitlines() if needle in l]


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("-"):
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    opts = dict(zip(args[1::2], args[2::2]))
    server = opts.get("--server")
    if not server:
        print("--server HOST:PORT is required (a team server with session tokens)")
        return 2
    server_log = opts.get("--server-log")
    editor = pathlib.Path(opts.get("--editor", HERE.parent / "out/build/release/src/HE_Editor/HorizonEditor"))
    timeout = int(opts.get("--timeout", "600"))
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)

    team = "tok-%d" % (int(time.time()) % 100000000)
    failures, transcript = [], {"server": server, "team": team}

    def expect(cond, what):
        log(("  ok   " if cond else "  FAIL ") + what)
        if not cond:
            failures.append(what)
        return cond

    def team_marker(needle):
        """Server log lines of this team's Bob containing `needle` (None without a log)."""
        lines = server_lines(server_log, needle)
        return None if lines is None else [l for l in lines if "team=%s member=Bob]" % team in l]

    def server_says(needle, before, what):
        lines = team_marker(needle)
        if lines is None:
            log("  --   (no server log) %s" % what)
            return
        expect(len(lines) > before, "server: %s (%d → %d × '%s')" % (what, before, len(lines), needle))

    def count(needle):
        lines = team_marker(needle)
        return 0 if lines is None else len(lines)

    # The team: Anna creates it, adds Bob. Bob is the editor's member throughout.
    admin = Device(server, team, "Anna", ANNA_PW, create=True)
    result, text = admin.request(ADD_MEMBER, s8("Bob") + s8(BOB_PW) + bytes([0]))
    admin.close()
    if not expect(result == 0, "team %s created by Anna, Bob added (%s)" % (team, text or "ok")):
        print("RESULT: FAIL")
        return 1

    p = Profile(outdir, editor, timeout, server, team, "Bob")
    try:
        # ── 1. Password sign-in, token on disk, clean quit keeps it ──────────
        p.start()
        expect(p.status_is("signed_out"), "start 1: signed out (no saved sign-in yet)")
        issued = count("was issued a session token")
        p.call("team_sign_in", {"server": server, "team": team, "member": "Bob", "password": BOB_PW})
        st, dt = wait_for("signed in", lambda: p.status_is("signed_in"), TIMEOUT)
        expect(st, "start 1: signed in with the password (%.1fs)" % dt)
        got, dt = wait_for("token file", lambda: p.token.exists(), TIMEOUT)
        expect(got, "start 1: team-session.txt written (%.1fs)" % dt)
        if p.token.exists():
            mode = stat.S_IMODE(p.token.stat().st_mode)
            expect(mode == 0o600, "team-session.txt is owner-only (%o)" % mode)
            body = p.token.read_text()
            transcript["token_file_lines"] = len(body.splitlines())
            expect(BOB_PW not in body, "the password is not in team-session.txt")
        server_says("was issued a session token", issued, "issued Bob a token")
        expect(p.quit() == 0, "start 1: clean quit")
        expect(p.token.exists(), "quit kept team-session.txt (disconnect, not Sign Out)")
        expect(BOB_PW not in p.config.read_text(), "the password is not in config.json")
        expect(p.config_value("TeamName") == team and p.config_value("TeamMember") == "Bob",
               "config.json still names team and member after the quit")

        # ── 2. Restart: signed in with no password; then Sign Out ───────────
        token_logins = count("logged in with a session token")
        p.start()
        st, dt = wait_for("resumed", lambda: p.status_is("signed_in"), TIMEOUT)
        expect(st and st.get("member") == "Bob" and st.get("team") == team,
               "start 2: signed in on its own, no team_sign_in (%.1fs)" % dt)
        server_says("logged in with a session token", token_logins, "Bob logged in with the token")
        revoked = count("revoked a session token")
        p.call("team_sign_out")
        expect(p.status_is("signed_out"), "start 2: Sign Out → signed out")
        expect(not p.token.exists(), "Sign Out deleted team-session.txt")
        time.sleep(1.0)       # the revoke goes out on the connection kept in m_leaving
        server_says("revoked a session token", revoked, "the server revoked the token on Sign Out")
        expect(p.quit() == 0, "start 2: clean quit")
        # The editor's stdout is block-buffered into the file: read it after the quit.
        expect("signed in to '%s' as 'Bob' with the saved session" % team in p.log_text(),
               "editor log 2: 'signed in … with the saved session'")

        # ── 3. Negative control: no file, no sign-in ────────────────────────
        p.start()
        time.sleep(5.0)
        st = p.team()
        expect(st.get("status") == "signed_out", "start 3: without the file it stays signed out (%s)"
               % st.get("status"))
        p.call("team_sign_in", {"server": server, "team": team, "member": "Bob", "password": BOB_PW})
        st, _ = wait_for("signed in 3", lambda: p.status_is("signed_in"), TIMEOUT)
        got, _ = wait_for("token file 3", lambda: p.token.exists(), TIMEOUT)
        expect(st and got, "start 3: signed in with the password, new token saved")
        expect(p.quit() == 0, "start 3: clean quit")

        # ── 4. Live revocation: Sign Out Everywhere on another device ───────
        token_logins = count("logged in with a session token")
        p.start()
        st, dt = wait_for("resumed 4", lambda: p.status_is("signed_in"), TIMEOUT)
        expect(st, "start 4: signed in with the token (%.1fs)" % dt)
        server_says("logged in with a session token", token_logins, "start 4 used the token")
        other = Device(server, team, "Bob", BOB_PW)
        result, text = other.request(REVOKE_ALL_TOKENS)
        other.close()
        expect(result == 0, "another device of Bob: Sign Out Everywhere (%s)" % text)
        st, dt = wait_for("kicked", lambda: p.status_is("failed"), TIMEOUT)
        transcript["live_revoke_error"] = (st or {}).get("error")
        expect(st and "no longer valid" in st.get("error", ""),
               "start 4: the live editor is signed out: %s (%.1fs)" % ((st or {}).get("error"), dt))
        expect(not p.token.exists(), "the revoked token's file is gone")
        p.call("team_sign_in", {"server": server, "team": team, "member": "Bob", "password": BOB_PW})
        st, _ = wait_for("signed in 4", lambda: p.status_is("signed_in"), TIMEOUT)
        got, _ = wait_for("token file 4", lambda: p.token.exists(), TIMEOUT)
        expect(st and got, "start 4: signed in again with the password, new token saved")
        expect(p.quit() == 0, "start 4: clean quit")

        # ── 5. Removed while the editor was off: refused at start ───────────
        admin = Device(server, team, "Anna", ANNA_PW)
        result, text = admin.request(REMOVE_MEMBER, s8("Bob"))
        admin.close()
        expect(result == 0, "Anna removes Bob while the editor is off (%s)" % (text or "ok"))
        p.start()
        st, dt = wait_for("refused at start", lambda: p.status_is("failed"), TIMEOUT)
        transcript["start_refused_error"] = (st or {}).get("error")
        expect(st and "no longer valid" in st.get("error", ""),
               "start 5: the saved sign-in is refused: %s (%.1fs)" % ((st or {}).get("error"), dt))
        expect(not p.token.exists(), "start 5: the refused token's file is gone")
        time.sleep(3.0)
        expect(p.status_is("failed"), "start 5: no retry with the refused token")
        expect(p.quit() == 0, "start 5: clean quit")
        expect("Team: the saved session was refused" in p.log_text(),
               "editor log 5: 'the saved session was refused'")
        expect(p.log_text().count("with a session token") == 1, "editor log 5: the token was offered once")
    except Exception as e:  # noqa: BLE001 — a crash is a failed run, logged with its reason
        failures.append("exception: %s" % e)
        log("  FAIL exception: %s" % e)
    finally:
        if p.proc and p.proc.poll() is None:
            p.proc.kill()
            p.proc.wait(5)

    transcript["failures"] = failures
    (outdir / "transcript.json").write_text(json.dumps(transcript, indent=2))
    print("RESULT: %s" % ("PASS" if not failures else "FAIL"))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
