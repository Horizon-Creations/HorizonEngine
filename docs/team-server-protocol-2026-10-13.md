# Team server: data model, login protocol (topic 197, server side)

Status (2026-10-13): **both server steps done**: teams, members (name + PBKDF2
password hash) and a login protocol (step 8), then presence, team sessions,
invitations and the per-session join setting (step 9) live in the relay server
(repo `Horizon-Creations/CollabSession`, branch `claude/he-net-relay-server`, on
the Mac at `~/VSCode/EngineCollab/CollabSession`). Accounts persist in
`--data-dir/teams.txt`; presence, sessions and invitations are memory only.
The engine side (`HE_Net` team subsystem) has not started.
The relay protocol (`docs/relay-server-protocol.md`) is unchanged.

The full specification is the README section "Team-Server" in that repo. In short,
for whoever writes the `HE_Net` client:

- Same port as the relay (7777), same framing `[u32 BE length][payload]`, same
  `"HERL" | 1 | type` header, protocol version stays 1. A team connection is a
  separate TCP connection whose **first frame is TEAM_LOGIN (type 6)** instead of HELLO.
- `TEAM_LOGIN: op u8 (1 login, 2 create team) | team str8 | member str8 | password str8`.
  Answer: STATUS `logged-in` (4), or a refusal followed by close: `bad-login` 23 (team,
  member or password wrong, deliberately not told apart; also "removed from the team"),
  `team-exists` 24, `invalid` 25 (name/password rules, text says which), `full` 21 (team
  limit), `server-error` 26, `bad-hello` 16, `bad-version` 17.
- After `logged-in` the connection stays open: `TEAM_REQUEST (7): request u32 | op u8 | fields`,
  `TEAM_REPLY (8): request u32 | result u8 | text str8 | body`. Ops: 1 LIST_MEMBERS
  (body `count u16` + `count × (name str8, flags u8)`, flags bit0 admin, bit1 "you"),
  2 ADD_MEMBER `name, password, admin u8` (admins only), 3 REMOVE_MEMBER `name` (admins),
  4 SET_PASSWORD `old, new`. Results 0 ok … 10 server-error (table in the README); a
  rejected request keeps the connection open.
- Names: 1-64 of `[A-Za-z0-9_-]`, case-insensitive (display keeps the first spelling).
  Passwords 8-255 bytes. The team's creator is its admin; a team keeps at least one admin.

Step 9 additions (README sections "Anwesenheit", "Team-Sitzungen", "Ereignisse"):

- **Presence:** online = at least one team connection (per member, not per connection).
  LIST_MEMBERS flag bit2 = online. A connection silent for `--team-idle-timeout` (60 s)
  gets STATUS `timeout` (20) and is closed, so the client sends op 5 PING every ~20 s.
- **Ops 6-12:** OPEN_SESSION `session, peer, open u8`; CLOSE_SESSION `session` (host);
  SET_SESSION_OPEN `session, open u8` (host); LIST_SESSIONS (body `count u16` +
  `count × (session, host, peer str8, flags u8)`, bit0 open, bit1 you're invited, bit2
  this connection hosts); INVITE `session, member` (host, member online); DECLINE
  `session`; JOIN `session` (open or invited; body `session, host, peer`). New results
  11 not-online, 12 no-such-session, 13 not-host, 14 not-allowed, 15 session-exists,
  16 not-invited. "Host" = the announcing *connection*, not the member.
- **TEAM_EVENT (type 9)** `kind u8 | fields`, pushed any time after logged-in, between
  replies: 1 presence `member, online u8`; 2 members-changed; 3 session `session, host,
  peer, open u8`; 4 session-closed `session`; 5 invited `session, host, peer` (to every
  connection of the invitee); 6 joining / 7 declined `session, member` (to the host
  connection). The causing connection never gets its own event.
- **Building the collab session:** host OPEN_SESSION then relay LISTEN (same session and
  peer id); joiner JOIN then relay CONNECT (session, own peer id = member name
  recommended, target = host peer). The relay does not check team membership; the
  random session id is the secret, as before.
- **Lifetime:** a session ends with its announcing connection; an invitation ends with
  the session or when the invitee goes offline; removing a member closes its
  connections and sessions. Limits 8 sessions per connection, 64 per team.

## Handoff

- **Topic / step:** 197, step 9 (presence, invite, join setting; plan step 2, retry)
- **Branch / commit:** CollabSession `claude/he-net-relay-server` @ 8e97b36 (pushed,
  one commit on a555f14; 8 commits ahead of CollabSession `main`, no PR). This doc: HE
  `claude/team-server-teams-mitglieder-anwesenheit`.
- **Done:** everything in "Step 9 additions" above.
- **Files touched (CollabSession):** `src/Protocol.{h,cpp}` (Type::TeamEvent, TeamOp 5-12,
  TeamResult 11-16, TeamEventKind, flags, `MakeSessionList`/`MakeJoinReply`/event
  builders), `src/RelayServer.{h,cpp}` (`HandleSessionRequest`, `LeaveTeam` from
  `Detach`, `EndSession`, `ForgetInvites`, `Push`/`PushTeam` with high-water drop,
  `IsOnline`, `lastHeard` + `CheckTimeouts`), `src/main.cpp` (`--team-idle-timeout`),
  `tests/team_test.py` (event-aware reader, 6 new tests), `README.md`.
- **Verified:** `ctest --test-dir /tmp/collab-build` 3/3 (team_protocol 18/18, relay_protocol
  unchanged), same 3/3 in an ASan/UBSan `-fno-sanitize-recover=all` build; three negative
  controls on a copy (no ForgetInvites on offline, no event high-water drop, no idle
  timeout) each fail exactly their test.
- **Not verified:** CollabSession CI run 38472456102 was still running at handoff
  (https://github.com/Horizon-Creations/CollabSession/actions/runs/38472456102); nothing
  deployed to 212.227.140.164 (step 5).
- **Decisions taken:** join setting per *session* (live, not persisted; teams.txt format
  unchanged); team server does not police relay pairing; host = connection; one invite
  per member (decline from any of its connections clears it); events to the causing
  connection are suppressed; the host may not JOIN its own session (bad-request).
- **Suggested, not taken:** a persisted per-team default "others may join" (needs a
  teams.txt format change + old-format load test); tying relay CONNECT to a JOIN ticket.
- **Traps:** events interleave with replies: a client (and tests) must read frames by
  type, never assume the next frame is the reply. `team_test.py` keeps them in `EVENTS`.
  The previous step's traps still apply (build in `/tmp/collab-build`, `python3 -B`, don't
  switch the human's checkout branch).
- **Next steps:** 1. Plan step 3: `HE_Net` team client (login, PING every 20 s, event
  reader, LIST_*/OPEN/INVITE/JOIN → existing relay transport). 2. Editor Team page and
  panel. 3. Deploy with `deploy/deploy.sh horizon@212.227.140.164 7777`. 4. E2E.
