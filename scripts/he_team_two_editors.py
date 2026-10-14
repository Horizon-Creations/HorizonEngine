#!/usr/bin/env python3
"""he_team_two_editors — two real editor processes, two members of one team.

tests/test_net_team.cpp and tests/test_team_ui.cpp put both editors' TeamControllers
into ONE process. This is the run they cannot be (topic 197, step 6): two deployed
HorizonEditor processes, each with its own HOME, project copy and MCP bridge, signed
in to a real team server (by default the rented host) as DIFFERENT members of a
fresh team, and the collab session built from there. Every check is read in the
editor that did NOT cause it, through that editor's own tools.

The team_* tools exist only with HE_MCP_COLLAB_CONTROL=1 and call the TeamController
methods the Team page and panel buttons call. HE_COLLAB_OFFLINE=1 keeps the router,
the LAN beacon and the public directory out of it; the team server is also the relay.

What it checks:
  1. both editors boot with the project and list the team tools;
  2. A creates team e2e-<time> as Anna (admin), adds Bob; B signs in as Bob;
     each sees the other `online`, Anna admin, Bob not;
  3. invitation: A hosts invitation-only; B cannot join it uninvited; A invites
     Bob, B lists it `invited`, B joins → B `joined`, A's roster names both, B holds
     A's pre-join marker (the snapshot) and sees an entity A creates afterwards;
  4. B declines a second invitation → its invitation is gone;
  5. open session: A hosts open, B joins WITHOUT an invitation → joined;
  6. the other way round: B (not an admin) hosts, invites Anna, A joins → joined;
  7. B signs out → A sees Bob offline.

Usage:
    scripts/he_team_two_editors.py OUTDIR [--server HOST:PORT] [--editor PATH] [--timeout S]

Each run leaves its team e2e-<time> on the server (the protocol has no delete-team).
Prints `RESULT: PASS|FAIL` last; both editor logs and a transcript go into OUTDIR.
"""

import json
import os
import pathlib
import secrets
import shutil
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import he_mcp_multiclient as mc  # noqa: E402
from he_collab_two_editors import Peer, wait_for, roster_names  # noqa: E402

log = mc.log

SERVER = "212.227.140.164:7777"
TEAM_TOOLS = ("team_status", "team_sign_in", "team_sign_out", "team_add_member", "team_host",
              "team_set_open", "team_invite", "team_join", "team_decline")
# Through a relay on the internet a join takes longer than over loopback.
TIMEOUT = 45.0


def team(p):
    return p.call("team_status")


def member(status, name):
    for m in status.get("members", []):
        if m.get("name", "").lower() == name.lower():
            return m
    return None


def session(status, sid):
    for s in status.get("sessions", []):
        if s.get("session") == sid:
            return s
    return None


def collab_is(p, want):
    st = p.status()
    if st.get("status") == "failed":
        log("  %s collab failed: %s" % (p.tag, st.get("error")))
    return st if st.get("status") == want else None


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    editor_path = mc.EDITOR
    server = SERVER
    timeout = int(os.environ.get("HE_SHOT_TIMEOUT", "1200"))
    for i, a in enumerate(args):
        if a == "--editor":
            editor_path = pathlib.Path(args[i + 1])
        if a == "--server":
            server = args[i + 1]
        if a == "--timeout":
            timeout = int(args[i + 1])
    if not editor_path.exists():
        log("no editor at %s" % editor_path)
        return 2
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)

    os.environ["HE_MCP_COLLAB_CONTROL"] = "1"
    os.environ["HE_COLLAB_OFFLINE"] = "1"

    team_name = "e2e-%d" % int(time.time())
    pw_anna, pw_bob = secrets.token_hex(8), secrets.token_hex(8)
    failures = []
    transcript = {"server": server, "team": team_name}

    def expect(cond, what):
        mc.expect(bool(cond), what, failures)

    def leave_both(a, b):
        b.call("collab_leave")
        a.call("collab_leave")
        wait_for("both idle", lambda: collab_is(a, "idle") and collab_is(b, "idle"), TIMEOUT)

    a = b = None
    try:
        a = Peer("A", outdir / "a", editor_path, timeout)
        b = Peer("B", outdir / "b", editor_path, timeout)
        t0 = time.time()
        a.connect()
        b.connect()
        log("both endpoints up after %.0fs" % (time.time() - t0))

        # ── 1. Up, with the project and the team tools ───────────────────────
        for p in (a, b):
            names = [t["name"] for t in p.client.bridge.request("tools/list")
                     .get("result", {}).get("tools", [])]
            info = p.call("scene_info")
            expect(info.get("sceneOpen"), "%s has a scene open" % p.tag)
            expect(all(n in names for n in TEAM_TOOLS), "%s lists every team tool" % p.tag)
            expect(team(p).get("status") == "signed_out", "%s starts signed out" % p.tag)
        if failures:
            return 1

        # ── 2. One team, two members, presence both ways ─────────────────────
        a.call("team_sign_in", {"server": server, "team": team_name, "member": "Anna",
                                "password": pw_anna, "create": True})
        st, dt = wait_for("A signed in", lambda: (lambda s: s if s.get("status") in
                                                  ("signed_in", "failed") else None)(team(a)), TIMEOUT)
        expect(st and st.get("status") == "signed_in",
               "A created %s as Anna (%.1fs) %s" % (team_name, dt, st and st.get("error", "")))
        if not st or st.get("status") != "signed_in":
            return 1
        a.call("team_add_member", {"name": "Bob", "password": pw_bob})
        st, dt = wait_for("Bob in A's roster", lambda: member(team(a), "Bob"), TIMEOUT)
        expect(st and not st.get("online"), "A added Bob, offline so far (%.1fs)" % dt)

        b.call("team_sign_in", {"server": server, "team": team_name, "member": "Bob",
                                "password": pw_bob})
        st, dt = wait_for("B signed in", lambda: (lambda s: s if s.get("status") in
                                                  ("signed_in", "failed") else None)(team(b)), TIMEOUT)
        expect(st and st.get("status") == "signed_in",
               "B signed in as Bob (%.1fs) %s" % (dt, st and st.get("error", "")))
        if not st or st.get("status") != "signed_in":
            return 1
        got, dt = wait_for("A sees Bob online", lambda: (member(team(a), "Bob") or {}).get("online"), TIMEOUT)
        expect(got, "A sees Bob online (%.1fs)" % dt)
        sb = team(b)
        expect((member(sb, "Anna") or {}).get("online"), "B sees Anna online")
        expect((member(sb, "Anna") or {}).get("admin") and not sb.get("isAdmin"),
               "B sees Anna as admin and is none itself")
        expect((member(sb, "Bob") or {}).get("you") and sb.get("member") == "Bob",
               "B is Bob in team %s" % sb.get("team"))
        transcript["presence_a"], transcript["presence_b"] = team(a), sb

        # ── 3. Invitation ─────────────────────────────────────────────────────
        marker = a.call("entity_create", {"name": "TeamMarkerA", "position": [2.0, 0.5, -1.0]})
        h = a.call("team_host", {"open": False})
        sid = h.get("hostedSession", "")
        expect(sid and collab_is(a, "hosting"), "A hosts invitation-only session %s" % sid)
        seen, _ = wait_for("B sees A's session", lambda: session(team(b), sid), TIMEOUT)
        if seen:
            expect(not seen.get("open") and not seen.get("invited"),
                   "B lists it as neither open nor invited: %s" % seen)
            b.call("team_join", {"session": sid})
            time.sleep(5.0)
            expect(not collab_is(b, "joined") and len(a.status().get("participants", [])) == 1,
                   "uninvited, B's join is refused (B %s)" % b.status().get("status"))
            b.call("collab_leave")
        else:
            log("  B does not list the session before the invite (server hides it)")
        a.call("team_invite", {"member": "Bob"})
        got, dt = wait_for("B invited", lambda: (session(team(b), sid) or {}).get("invited"), TIMEOUT)
        expect(got, "B lists A's session as invited (%.1fs)" % dt)
        b.call("team_join", {"session": sid})
        st, dt = wait_for("B joined", lambda: collab_is(b, "joined"), TIMEOUT)
        expect(st, "B joined A's session through the invitation (%.1fs)" % dt)
        st_a, _ = wait_for("A's roster 2", lambda: (lambda s: s if len(s.get("participants", [])) == 2
                                                    else None)(a.status()), TIMEOUT)
        expect(st_a and roster_names(st_a) == ["Anna", "Bob"],
               "A's roster names Anna and Bob: %s" % (st_a and roster_names(st_a)))
        transcript["invited_roster_a"] = st_a
        expect(marker["uuid"] in b.entities(), "B holds A's pre-join marker (the snapshot)")
        live = a.call("entity_create", {"name": "LiveFromA", "position": [1.0, 2.0, 3.0]})
        got, dt = wait_for("B sees LiveFromA", lambda: live["uuid"] in b.entities(), TIMEOUT)
        expect(got, "B sees A's new entity under the same uuid (%.1fs)" % dt)
        leave_both(a, b)

        # ── 4. Decline ────────────────────────────────────────────────────────
        h = a.call("team_host", {"open": False})
        sid = h.get("hostedSession", "")
        a.call("team_invite", {"member": "Bob"})
        got, _ = wait_for("B invited again", lambda: (session(team(b), sid) or {}).get("invited"), TIMEOUT)
        expect(got, "B is invited to %s" % sid)
        b.call("team_decline", {"session": sid})
        got, dt = wait_for("invitation gone", lambda: not (session(team(b), sid) or {}).get("invited"), TIMEOUT)
        expect(got, "after Decline B's invitation is gone (%.1fs)" % dt)
        expect(not collab_is(b, "joined"), "declining did not join")
        leave_both(a, b)

        # ── 5. Open session: join without an invitation ───────────────────────
        h = a.call("team_host", {"open": True})
        sid = h.get("hostedSession", "")
        got, dt = wait_for("B sees open", lambda: (session(team(b), sid) or {}).get("open"), TIMEOUT)
        expect(got and not (session(team(b), sid) or {}).get("invited"),
               "B lists A's session as open, not invited (%.1fs)" % dt)
        b.call("team_join", {"session": sid})
        st, dt = wait_for("B joined open", lambda: collab_is(b, "joined"), TIMEOUT)
        expect(st, "B joined the open session without an invitation (%.1fs)" % dt)
        leave_both(a, b)

        # ── 6. The other way round: Bob hosts, invites Anna ───────────────────
        h = b.call("team_host", {"open": False})
        sid = h.get("hostedSession", "")
        expect(sid and collab_is(b, "hosting"), "B (no admin) hosts %s" % sid)
        b.call("team_invite", {"member": "Anna"})
        got, dt = wait_for("A invited", lambda: (session(team(a), sid) or {}).get("invited"), TIMEOUT)
        expect(got, "A lists B's session as invited (%.1fs)" % dt)
        a.call("team_join", {"session": sid})
        st, dt = wait_for("A joined", lambda: collab_is(a, "joined"), TIMEOUT)
        expect(st, "A joined Bob's session (%.1fs)" % dt)
        st_b, _ = wait_for("B's roster 2", lambda: (lambda s: s if len(s.get("participants", [])) == 2
                                                    else None)(b.status()), TIMEOUT)
        expect(st_b and roster_names(st_b) == ["Anna", "Bob"],
               "B's roster names Anna and Bob: %s" % (st_b and roster_names(st_b)))
        a.call("collab_leave")
        b.call("collab_leave")

        # ── 7. Sign out → offline on the other side ──────────────────────────
        b.call("team_sign_out")
        got, dt = wait_for("A sees Bob offline",
                           lambda: (member(team(a), "Bob") or {"online": True}).get("online") is False,
                           TIMEOUT)
        expect(got, "after B signs out A sees Bob offline (%.1fs)" % dt)
        a.call("team_sign_out")
    except Exception as exc:
        log("ERROR: %s" % exc)
        failures.append("exception: %s" % exc)
    finally:
        for p in (a, b):
            if p is None:
                continue
            if p.client:
                p.client.close()
            text = p.ed.log_text()
            p.ed.stop()
            lines = [l for l in text.splitlines() if "Team" in l or "Collab" in l or "MCP: team" in l]
            (outdir / ("team_log_%s.txt" % p.tag.lower())).write_text("\n".join(lines))
            log("%s editor log: %d team/collab lines" % (p.tag, len(lines)))
        (outdir / "transcript.json").write_text(json.dumps(transcript, indent=1, default=str))

    log("team left on %s: %s" % (server, team_name))
    log("RESULT: %s (%d failures)" % ("PASS" if not failures else "FAIL", len(failures)))
    for f in failures:
        log("  - " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
