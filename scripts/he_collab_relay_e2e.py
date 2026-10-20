#!/usr/bin/env python3
"""he_collab_relay_e2e — one host and several guests, real editor processes,
through a relay server or peer-to-peer (Thema 193, Schritt 5).

The relay cases in tests/test_net_relay.cpp put every CollabController into ONE
process. This is the run they cannot be: N+1 deployed HorizonEditor processes,
each with its own HOME and project copy, driven over MCP, sharing one
collaboration session. With --relay every editor gets HE_NET_RELAY, so all of
them only dial OUT to the relay; with --p2p (HE_NET_RELAY=off) the host opens a
port and the guests connect to it directly, as before the relay existed.

What it checks, each change from the side that did NOT make it:
  1. every editor boots with the project and the collab tools;
  2. A hosts; through a relay A opens NO listening socket besides MCP (lsof),
     peer-to-peer it does;
  3. every guest joins (by session id through the relay, by address P2P), all
     rosters name everybody;
  4. broadcast: A creates an entity → every guest sees it (one message to
     several clients: through the relay a BROADCAST frame, the relay copies);
  5. unicast: guest B moves it → A and the other guests see it (B → host over
     its pair, host → the rest);
  6. guest C creates an entity → A and B see it;
  7. witnesses: the relay log (local relay only) names the pairs, every
     guest's subscription and its "received N broadcast bytes", and the
     listener's "forwarded … in K broadcasts" with K > 0; A's editor log says
     every guest "receives broadcasts through the relay"; lsof shows every new
     session link of every editor going out to the relay; P2P runs leave the
     relay log untouched;
  8. guests leave → A's roster shrinks back to A.

Usage:
    scripts/he_collab_relay_e2e.py OUTDIR (--relay HOST:PORT [--relay-log FILE] | --p2p)
                                   [--guests N] [--editor PATH] [--timeout S]

--relay-log names the log of a relay running on this machine (collab_server's
stdout); the run only reads the lines written while it ran. Prints
`RESULT: PASS|FAIL` last and writes every editor's log and a transcript into
OUTDIR.
"""

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import he_mcp_multiclient as mc  # noqa: E402
from he_collab_two_editors import Peer, wait_for, close, world_pos, roster_names  # noqa: E402

log = mc.log


def listening_ports(pid):
    """TCP ports the process listens on (lsof), as ints."""
    r = subprocess.run(["/usr/sbin/lsof", "-nP", "-a", "-p", str(pid), "-iTCP", "-sTCP:LISTEN"],
                       capture_output=True, text=True)
    ports = set()
    for line in r.stdout.splitlines()[1:]:
        m = re.search(r":(\d+) \(LISTEN\)", line)
        if m:
            ports.add(int(m.group(1)))
    return ports


def tcp_links(pid):
    """The process's established TCP connections as (local, remote) strings."""
    r = subprocess.run(["/usr/sbin/lsof", "-nP", "-a", "-p", str(pid), "-iTCP", "-sTCP:ESTABLISHED"],
                       capture_output=True, text=True)
    out = set()
    for line in r.stdout.splitlines()[1:]:
        m = re.search(r"(\S+)->(\S+) \(ESTABLISHED\)", line)
        if m:
            out.add((m.group(1), m.group(2)))
    return out


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    outdir = pathlib.Path(args[0]).resolve()
    editor_path = mc.EDITOR
    timeout = int(os.environ.get("HE_SHOT_TIMEOUT", "1200"))
    relay = None
    relay_log = None
    p2p = False
    guests = 2
    for i, a in enumerate(args):
        if a == "--editor":
            editor_path = pathlib.Path(args[i + 1])
        if a == "--timeout":
            timeout = int(args[i + 1])
        if a == "--relay":
            relay = args[i + 1]
        if a == "--relay-log":
            relay_log = pathlib.Path(args[i + 1])
        if a == "--p2p":
            p2p = True
        if a == "--guests":
            guests = int(args[i + 1])
    if p2p == (relay is not None):
        log("give exactly one of --relay HOST:PORT and --p2p")
        return 2
    if guests < 2:
        # One guest never reaches the broadcast path (a fan-out to a single
        # recipient is a plain send), so it would prove nothing about it.
        log("--guests must be at least 2")
        return 2
    if not editor_path.exists():
        log("no editor at %s" % editor_path)
        return 2
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)

    os.environ["HE_MCP_COLLAB_CONTROL"] = "1"
    os.environ["HE_COLLAB_OFFLINE"] = "1"
    # Read once per process at start; "off" forces peer-to-peer even if a
    # relay sits in a config somewhere.
    os.environ["HE_NET_RELAY"] = relay if relay else "off"
    # The fan-out's Info lines are the editor-side witness.
    os.environ["HE_LOG"] = "Net=Info"
    mode = "relay %s" % relay if relay else "peer-to-peer"
    log("mode: %s, 1 host + %d guests" % (mode, guests))

    relay_log_start = relay_log.stat().st_size if relay_log and relay_log.exists() else 0

    failures = []
    transcript = {"mode": mode, "guests": guests}

    def expect(cond, what):
        mc.expect(bool(cond), what, failures)

    peers = []
    try:
        a = Peer("A", outdir / "a", editor_path, timeout)
        peers.append(a)
        gs = []
        for i in range(guests):
            tag = chr(ord("B") + i)
            gs.append(Peer(tag, outdir / tag.lower(), editor_path, timeout))
        peers.extend(gs)
        t0 = time.time()
        for p in peers:
            p.connect()
        log("%d endpoints up after %.0fs" % (len(peers), time.time() - t0))

        # ── 1. Up, with the project and the tools ────────────────────────────
        for p in peers:
            names = [t["name"] for t in p.client.bridge.request("tools/list")
                     .get("result", {}).get("tools", [])]
            info = p.call("scene_info")
            expect(info.get("sceneOpen"), "%s has a scene open" % p.tag)
            expect(all(n in names for n in ("collab_status", "collab_host", "collab_join",
                                            "collab_leave")),
                   "%s lists collab_status/host/join/leave" % p.tag)
            expect(p.status().get("status") == "idle", "%s starts idle" % p.tag)
        if failures:
            return 1
        listen_before = listening_ports(a.ed.proc.pid)
        # The editor has loopback links of its own before any session (the MCP
        # bridge, an internal loopback listener); only what is new afterwards
        # belongs to the session.
        links_before = {p.tag: tcp_links(p.ed.proc.pid) for p in peers}

        # ── 2. Host ──────────────────────────────────────────────────────────
        host = a.call("collab_host", {"name": "Host A"})
        transcript["collab_host"] = {k: v for k, v in host.items() if k != "joinCode"}
        expect(host.get("status") == "hosting", "A is hosting")
        sid = host.get("sessionId", "")
        log("A hosting: session %s, port %s" % (sid, host.get("port")))
        if relay:
            # The relay has to take the listener before anybody can join
            # (no-such-peer otherwise); collab_status does not show that, so
            # give the editor's frames a moment.
            time.sleep(1.5)
        new_listen = listening_ports(a.ed.proc.pid) - listen_before
        transcript["host_new_listen_ports"] = sorted(new_listen)
        if relay:
            expect(not new_listen, "through the relay A opens no listening port (new: %s)"
                   % sorted(new_listen))
            expect(sid, "A has a session id to share")
        else:
            expect(host.get("port") in new_listen,
                   "peer-to-peer A listens on its session port %s" % host.get("port"))

        # ── 3. Everybody joins ───────────────────────────────────────────────
        for g in gs:
            if relay:
                args_ = {"sessionId": sid, "joinCode": host["joinCode"], "name": "Guest " + g.tag}
            else:
                args_ = {"host": "127.0.0.1", "port": host["port"],
                         "joinCode": host["joinCode"], "name": "Guest " + g.tag}
            j = g.call("collab_join", args_)
            expect(j.get("status") in ("connecting", "joined"),
                   "%s's join started (%s)" % (g.tag, j.get("status")))
        want = sorted(["Host A"] + ["Guest " + g.tag for g in gs])
        for g in gs:
            def joined(g=g):
                st = g.status()
                if st.get("status") == "failed":
                    log("  %s failed: %s" % (g.tag, st.get("error")))
                    return st
                return st if st.get("status") == "joined" else None
            st, dt = wait_for("%s joined" % g.tag, joined)
            expect(st and st.get("status") == "joined", "%s reached `joined` (%.1fs)" % (g.tag, dt))
        if failures:
            return 1
        for p in peers:
            st, dt = wait_for("%s's roster complete" % p.tag,
                              lambda p=p: (lambda s: s if roster_names(s) == want else None)(p.status()))
            expect(st, "%s's roster names everybody: %s" % (p.tag, roster_names(p.status())))
            transcript["roster_" + p.tag] = roster_names(p.status())

        # Who each editor is connected to for the session: through the relay
        # nothing but the relay, every connection outgoing to it.
        for p in peers:
            new = sorted(tcp_links(p.ed.proc.pid) - links_before[p.tag])
            transcript["session_tcp_" + p.tag] = new
            if relay:
                rport = relay.rsplit(":", 1)[1]
                others = [l for l in new if not l[1].endswith(":" + rport)]
                expect(new and not others,
                       "%s's session links all go out to the relay (%d: %s; others: %s)"
                       % (p.tag, len(new), [l[1] for l in new], others))
            else:
                log("  %s session links: %s" % (p.tag, ["%s->%s" % l for l in new]))

        # ── 4. Broadcast: A creates → every guest sees it ────────────────────
        live_a = a.call("entity_create", {"name": "LiveFromA", "position": [1.0, 2.0, 3.0]})
        ua = live_a["uuid"]
        for g in gs:
            got, dt = wait_for("%s sees LiveFromA" % g.tag, lambda g=g: ua in g.entities())
            expect(got, "%s sees A's new entity (%.1fs)" % (g.tag, dt))
            expect(close(world_pos(g, ua), [1.0, 2.0, 3.0]),
                   "…at A's position on %s: %s" % (g.tag, world_pos(g, ua)))

        # ── 5. Unicast: B moves → A and the other guests see it ──────────────
        b = gs[0]
        ok, sc, pend = b.retry_lock("entity_set_transform", {"uuid": ua, "position": [-4.0, 1.0, 2.0]})
        expect(ok, "B's move went through after %d lock_pending retries (%s)" % (pend, sc.get("code", "ok")))
        for p in [a] + gs[1:]:
            got, dt = wait_for("%s sees B's move" % p.tag,
                               lambda p=p: close(world_pos(p, ua), [-4.0, 1.0, 2.0]))
            expect(got, "%s sees B's move (%.1fs): %s" % (p.tag, dt, world_pos(p, ua)))

        # ── 6. C creates → A and B see it ────────────────────────────────────
        c = gs[1]
        live_c = c.call("entity_create", {"name": "LiveFromC", "position": [0.0, 3.0, -1.0]})
        uc = live_c["uuid"]
        for p in [a, b] + gs[2:]:
            got, dt = wait_for("%s sees LiveFromC" % p.tag, lambda p=p: uc in p.entities())
            expect(got, "%s sees C's new entity (%.1fs)" % (p.tag, dt))

        # ── 8. Leaving ───────────────────────────────────────────────────────
        for g in gs:
            left = g.call("collab_leave")
            expect(left.get("inSession") is False, "%s is out of the session" % g.tag)
        st, dt = wait_for("A's roster back to 1",
                          lambda: (lambda s: s if len(s.get("participants", [])) == 1 else None)(a.status()))
        expect(st and roster_names(st) == ["Host A"], "A's roster is back to A alone (%.1fs)" % dt)
        a.call("collab_leave")
    except Exception as exc:  # a crash is a failure with its reason, not a traceback
        log("ERROR: %s" % exc)
        failures.append("exception: %s" % exc)
    finally:
        fan_lines = {}
        for p in peers:
            if p.client:
                p.client.close()
            text = p.ed.log_text()
            p.ed.stop()
            lines = [l for l in text.splitlines()
                     if "Collab" in l or "Relay" in l or "relay" in l or "MCP: collab" in l]
            (outdir / ("collab_log_%s.txt" % p.tag.lower())).write_text("\n".join(lines))
            fan_lines[p.tag] = [l.strip() for l in lines if "Relay fan-out" in l]
            log("%s editor log: %d collab/relay lines" % (p.tag, len(lines)))
        transcript["fanout_lines"] = fan_lines

    # ── 7. Witnesses ─────────────────────────────────────────────────────────
    # Only the host's log is read: a guest whose world the session replaced
    # does not end on SIGTERM (unsaved changes), stop() kills it, and its
    # block-buffered log is cut off. The guests' side is in the relay log.
    if relay:
        on_relay = [l for l in fan_lines.get("A", []) if "receives broadcasts through the relay" in l]
        expect(len(on_relay) >= guests,
               "A's log: %d of %d guests receive broadcasts through the relay"
               % (len(on_relay), guests))
    else:
        expect(not any(fan_lines.values()), "peer-to-peer: no relay fan-out in any editor log")
    if relay_log and relay_log.exists():
        time.sleep(1.0)
        with open(relay_log, "rb") as f:
            f.seek(relay_log_start)
            rl = f.read().decode("utf-8", "replace").splitlines()
        (outdir / "relay_log_during_run.txt").write_text("\n".join(rl))
        transcript["relay_log_lines"] = len(rl)
        if relay:
            paired = [l for l in rl if "paired" in l]
            casts = [int(m.group(1)) for l in rl
                     for m in [re.search(r"in (\d+) broadcasts", l)] if m]
            log("relay log: %d lines, %d 'paired', broadcasts per listener %s"
                % (len(rl), len(paired), casts))
            for l in rl:
                if "broadcasts" in l or "subscribe" in l.lower():
                    log("    " + l.strip())
            expect(len(paired) >= len([p for p in peers if p.tag != "A"]),
                   "relay log: every guest paired (%d)" % len(paired))
            expect(casts and max(casts) > 0,
                   "relay log: the host's listener forwarded broadcasts (%s)" % casts)
            subs = [l for l in rl if "] subscribed to #" in l]
            got = [int(m.group(1)) for l in rl
                   for m in [re.search(r"received (\d+) broadcast bytes", l)] if m]
            expect(len(subs) >= guests, "relay log: %d guests subscribed" % len(subs))
            expect(len(got) >= guests and min(got) > 0,
                   "relay log: every subscriber received broadcast bytes (%s)" % got)
        else:
            expect(not rl, "peer-to-peer: the relay saw nothing of this run (%d lines)" % len(rl))

    (outdir / "transcript.json").write_text(json.dumps(transcript, indent=1, default=str))
    log("RESULT: %s (%d failures)" % ("PASS" if not failures else "FAIL", len(failures)))
    for f in failures:
        log("  - " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
