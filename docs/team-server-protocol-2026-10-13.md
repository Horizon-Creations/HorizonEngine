# Team server: data model, login protocol (topic 197, server side)

Status (2026-10-13): **both server steps done**: teams, members (name + PBKDF2
password hash) and a login protocol (step 8), then presence, team sessions,
invitations and the per-session join setting (step 9) live in the relay server
(repo `Horizon-Creations/CollabSession`, branch `claude/he-net-relay-server`, on
the Mac at `~/VSCode/EngineCollab/CollabSession`). Accounts persist in
`--data-dir/teams.txt`; presence, sessions and invitations are memory only.
The engine side (step 10) is in: `HE::Net::TeamClient` and the editor's
`TeamController` (section "Engine side" below); the Team page and panel are not.
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

Server state after step 9: CollabSession `claude/he-net-relay-server` @ e3f4649 (pushed,
no PR), CI green on all three jobs
(https://github.com/Horizon-Creations/CollabSession/actions/runs/38472878828). Nothing
deployed yet. Server decisions: join setting per session (not persisted), the team server
does not police relay pairing, host = connection, events never go to the causing connection.

## Engine side (step 10)

- **`HE::Net::TeamClient`** (`src/HE_Net/include/Net/TeamClient.h`): one team connection
  over `TcpTransport`. `connect(server, team, member, password, Login|CreateTeam)`, then
  `update()` per frame. Every frame is dispatched by type (STATUS / TEAM_REPLY /
  TEAM_EVENT); replies are matched by request id and run their `ReplyFn` after the frame
  loop. Requests made before the link is up go out right behind TEAM_LOGIN. PING every
  20 s; no answer to the oldest request within 30 s (or no login within 30 s) closes it
  ("stopped answering"). Caches `members()` (admin/you/online) and `sessions()`
  (open/invited/hosting): fetched at login, refetched after our own changes (the server
  does not echo them) and on members-changed, patched by presence/session/invited events;
  `revision()` moves on every change. invited/joining/declined (and the rest) also go to
  `pollEvent()`. `loginRefused()` = bad-login/invalid/team-exists: do not retry.
- **Join code:** the JOIN reply carries no join code, so both ends derive it:
  `TeamClient::joinCodeForSession(sessionId)` (HMAC-SHA256 under "HorizonEngine team session
  v1", 32 hex). Whoever knows a team session's id can enter it: the members and the server.
- **`CollabController`**: `startTeamHosting(relay, name)` (startHosting with the relay forced
  to the team server and the derived code; listens as `kHostPeerId`) and
  `joinTeamSession(relay, session, hostPeer, selfPeer, name)` (CONNECT to `hostPeer` as
  `selfPeer`); `teamSession()`. The large-asset retry dials a team join again the same way.
- **`TeamController`** (`src/HE_Editor/TeamController.h`, ImGui-free): owns the TeamClient,
  `signIn`/`createTeam`/`signOut`, statuses SignedOut/Connecting/SignedIn/Reconnecting/
  Failed. Reconnects with backoff 1-30 s unless the login was refused (a refused first
  login or a dropped first connect is Failed, for the person to retry). Password in memory
  only. `hostSession(open)` = startTeamHosting + OPEN_SESSION (a refusal leaves the collab
  session again); `setSessionOpen`, `invite`, `join` (JOIN, then joinTeamSession with the
  member name as relay peer id), `decline`, `addMember`/`removeMember`/`changePassword`.
  When the collab session ends, it sends CLOSE_SESSION; after a reconnect it announces
  the still-running session again. Notices (invitations, joining, declined, refusals) via
  `pollNotice()` and the editor NotificationStore.
- **Editor:** `EditorApplication::m_team`, pumped right after `m_collab.update`, signed out
  at shutdown, `AppContext::team` for the panel. No settings page, no panel, no config
  fields yet (step 11).

## Handoff

- **Topic / step:** 197, step 10 (plan step 3, HE_Net team subsystem)
- **Branch / commit:** `claude/team-server-teams-mitglieder-anwesenheit` (pushed; commit in
  the Hive result post). Server unchanged: CollabSession e3f4649.
- **Done:** "Engine side (step 10)" above.
- **Files touched:** `src/HE_Net/{include/Net/TeamClient.h,src/TeamClient.cpp,CMakeLists.txt}`,
  `src/HE_Editor/TeamController.{h,cpp}` (+ editor CMakeLists), `src/HE_Editor/CollabController.{h,cpp}`
  (team entry points, `relayForNewSession` override, `beginLink` peer ids, `LastJoin`),
  `src/HE_Editor/EditorApplication.{h,cpp}` (m_team, AppContext::team),
  `tests/test_net_team.cpp`, `tests/CMakeLists.txt` (source list, `he_net` resource lock).
- **Verified (Mac, Release, worktree build `out/build/release`):** `he_tests
  --source-file=*test_net_team.cpp` 8/8 cases, 268 assertions, 8 runs in a row, with
  `HE_TEST_TEAM_SERVER=127.0.0.1:47791` pointing at a local `collab_server` built from
  e3f4649 (`--data-dir` in /tmp). The live case drives three real TeamController +
  CollabController pairs: create team, add members, presence both ways, invitation-only
  refusal, invite → decline → invite → join (scene arrives), open session joined by a
  never-invited member, CLOSE on leave, offline on sign-out; the server log shows each
  step and the relay pairing as peer `Bob`/`Cleo`. Regressions green: test_net_relay (27,
  also with HE_TEST_RELAY on the same server), test_collab_controller (74), test_net_collab
  (111), test_mcp_tools_collab (5), test_net_tcp (12). `HorizonEditor` and `he_tests` build
  green (rc 0) on the final state, after a `touch` of `TeamClient.h`.
- **Not verified:** against the rented host (step 5 not deployed); the editor GUI (no UI yet);
  Windows/Linux only through CI.
- **Decisions taken:** join code derived from the session id (above) instead of a server
  change; member name as the joiner's relay peer id (protocol recommendation); a second
  editor of the same member joining the same session gets the relay's id-taken (known,
  not handled); a refused announcement leaves the collab session; no config fields until
  the Team page exists.
- **Suggested, not taken:** host-side gate matching INCOMING peer ids against `joining`
  events (would make invitation-only binding on the relay too); keychain storage for the
  password so a restart signs in again.
- **Traps:** `ReplyFn` callbacks run inside `TeamClient::update()`: they must not destroy
  the client (TeamController never resets `m_client` from one). A fake server must skip
  frames queued behind a refused login. The server keeps an invitation after JOIN (a
  re-join logs "(invited)"), so the open-session path needs a never-invited member.
- **Next steps:** 1. Step 11: Preferences "Team" page (server address, team, member; replaces
  the Collaboration relay field) and a Team panel on `AppContext::team` (members with
  online dots, sessions, Invite/Join/Decline, admin add/remove), with EditorHelp entries.
  2. Deploy the server (`deploy/deploy.sh horizon@212.227.140.164 7777`). 3. E2E with two
  editors against the host; rerun `test_net_team` with `HE_TEST_TEAM_SERVER` pointed there.
