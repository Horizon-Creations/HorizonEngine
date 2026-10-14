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
  at shutdown, `AppContext::team` for the panel (UI: step 11 below).

## Editor UI (step 11)

Screenshots (headless, `tests/test_team_ui.cpp` with `HE_UI_DUMP_DIR`):
`docs/images/team-ui-2026-10-14/`. Fake team server: `team-page-signed-out.png`,
`team-page-signed-in.png`, `team-page-refused.png`, `team-panel-signed-in.png`,
`team-panel-hosting.png`. Real `collab_server` (CollabSession e3f4649, local), two editors:
`team-live-anna-hosting.png`, `team-live-bob-invited.png`, `team-live-bob-joined.png`.
The page shots are TeamView's body only (heading + body), not the Preferences rail.

- **`TeamView`** (`src/HE_Editor/TeamView.{h,cpp}`): the drawing half, over `TeamController`
  and three config strings, never `AppContext` (so he_tests draws it). `drawSettings` =
  Preferences ▸ Team body; `drawPanel` = Window ▸ Team body (signed out: the same sign-in
  form). Help scope "Team" (entries in `EditorHelp.cpp`, ref group "Team"). The session id
  is never displayed (it is the key, see joinCodeForSession). `setProbe()` is a test seam:
  each control's rect by label + row, so tests click "Invite" next to Bob.
- **Preferences:** page `Page::Team` replaces `CollabGeneral` ("Collaboration"). Top:
  Team Server field + Team/Member/Password + Sign In / Create Team / Sign Out + status;
  below, under "Sessions", the three catalog rows (category renamed "Collaboration" →
  "Team", help keys `Preferences/Team/...`). The Relay Server row is gone: **the Team
  Server field writes the same `EditorConfig::CollabRelayAddress`** (config key unchanged,
  so an existing relay address carries over and Collaboration-window sessions keep using
  it as relay). New config `TeamName`, `TeamMember`; never the password (wiped from the
  form the moment Sign In is pressed). Fields lock while a connection exists.
- **Window ▸ Team** (`EditorUI.cpp` `drawTeamWindow`, macOS menu `ToggleTeam`, persisted as
  `PanelOpenTeam`): status + Sign Out; members table (dot = online, ring = offline;
  Invite enabled only while hosting and for online members; admins get Remove → Confirm
  Remove); Sessions (Anyone in the Team May Join + Host Session, or "You are hosting" with
  the open toggle; the others' sessions with Join when open/invited, Decline when
  invited); Add Member (admins); Your Password (collapsed); Recent notices. A new
  invitation opens the window once.
- `tests/FakeTeamServer.h` (moved out of test_net_team.cpp) now applies ADD/REMOVE/INVITE/
  DECLINE, reports open|invited|hosting flags, and can ignore relay connections
  (`ignoreStrangers`).

## Handoff

- **Topic / step:** 197, step 11 (Team page + Team panel, screenshots).
- **Branch / commit:** `claude/team-server-teams-mitglieder-anwesenheit` (commit in the Hive
  result post). Server unchanged: CollabSession e3f4649, still not deployed.
- **Done:** "Editor UI (step 11)" above. `TeamController` gained `sessionRunning()` and
  `inSession(id)`; "Preferences > Collaboration" texts in CollabController/CollabPanel now
  say Team; `editor_help_audit.py` scans `TeamView.cpp` (collab 81/81).
- **Verified (Mac, Release, `out/build/release`):** `he_tests --source-file=*test_team_ui.cpp`
  3/3 cases, 221 assertions, 3 runs in a row with `HE_TEST_TEAM_SERVER=127.0.0.1:47792`
  (local collab_server, server log shows invite → "joins (invited)" → relay paired), and
  201 without it (live case skipped). Regressions green: test_net_team 8/8 (268, live too),
  test_editor_help 19/19, test_mcp_tools_settings 20/20, test_quick_settings_favorites,
  test_collab_controller 74/74, test_editor_dock_state, test_docs_library, test_ui_shot.
  `editor_help_audit.py --check` rc 0. All 8 PNGs looked at.
- **Not verified:** the real editor window by hand (Preferences rail, docking, the macOS
  menu item) — only the TeamView body is drawn headless; Windows/Linux only via CI; the
  rented host (step 5 not deployed).
- **Decisions:** one address for team server and relay (reuses `CollabRelayAddress`);
  sign-in lives on both surfaces (one `TeamView::editorForm()`); Restore Defaults clears
  the server field but keeps team/member names; a signed-in connection keeps its server.
- **Suggested, not taken:** keychain for the password (sign in again after restart);
  showing "invited" next to a member you invited (the server does not report it back).
- **Traps:** a member row vanishing (Remove) moves every control below it up: wait for the
  roster in UI tests before clicking the next field. The fake must not REQUIRE on relay
  HELLOs when a test presses Host/Join (`ignoreStrangers`).
- **Next steps:** 1. Deploy the server (`deploy/deploy.sh horizon@212.227.140.164 7777`,
  step 5). 2. E2E with two real editors against the host (step 6): Preferences ▸ Team →
  `212.227.140.164:7777`, Create Team / Add Member / Sign In, Window ▸ Team → Host,
  Invite, Join; rerun test_team_ui and test_net_team with `HE_TEST_TEAM_SERVER` there.
- **Step 5 (deploy), 2026-10-14: blocked, nothing deployed.** The host still runs
  f61c9f1 (relay only, unit without `--data-dir`). A worker session may not run
  `deploy/deploy.sh`: the auto-mode classifier denies it as "Production Deploy". The human
  runs it (`cd ~/VSCode/EngineCollab/CollabSession && deploy/deploy.sh
  horizon@212.227.140.164 7777`, HEAD e3f4649) or allows it. Baseline against the host
  before that: `HE_TEST_RELAY=212.227.140.164:7777 he_tests --source-file=*test_net_relay.cpp
  --test-case=*real collab_server*` 2/2 (79 assertions); the team live case of
  test_net_team fails at sign-in, as expected on the old server. After the deploy, check
  `~/collab-relay/bin/VERSION` = e3f4649 and `--data-dir` in the unit's ExecStart, then rerun
  both live cases (relay must stay 2/2, team must pass). Every live team run leaves a
  `he-test-N` team on the server (the protocol has no delete-team).
- **Step 16 (verify the human's deploy), 2026-10-14: deployed and live-green.** Read-only
  ssh: `~/collab-relay/bin/VERSION` = e3f4649 (built 05:38 UTC, `build-e3f4649`,
  `src-e3f4649` next to the old f61c9f1 dirs), user service `collab-relay` active since
  05:38:26 UTC, ExecStart `collab_server 7777 --data-dir /home/horizon/collab-relay/data`,
  startup log "team store .../teams.txt: 0 teams", PBKDF2 20000 iterations 7.9 ms per login.
  Live against `212.227.140.164:7777` from the Mac (`out/build/release`, up to date with
  HEAD): test_net_relay real collab_server 2/2 (79), test_net_team 8/8 (268) with
  `HE_TEST_TEAM_SERVER`, test_team_ui 3/3 (221) with it. The server journal proves the live
  cases hit the host: team he-test-598852236 created, Bob logged in, invite, decline,
  re-invite, "joins (invited)", relay "paired host -> Bob"; session set open, Cleo "joins
  (open)", paired; then the two-editor UI case (team uishot902353) invite → join → paired.
  Left on the server: teams he-test-598852236 and uishot902353 (no delete-team).
  Not verified: two real editor windows by hand against the host (step 6).
- **Step 6 (end-to-end acceptance), 2026-10-14: two editor processes, PASS.**
  - *Added:* team MCP tools in `src/HE_Editor/McpToolsCollab.cpp` (`registerTeamTools`,
    declared in `McpToolRegistry.h`, registered next to the collab tools in
    `EditorApplication.cpp`). `team_status` is always there; `team_sign_in` (`create`),
    `team_sign_out`, `team_add_member`, `team_host` (`open`), `team_set_open`, `team_invite`,
    `team_join` and `team_decline` only with `HE_MCP_COLLAB_CONTROL=1`. They call the same
    TeamController methods as the Team page/panel. Unit test: `test_mcp_tools_collab.cpp`
    "team tools: …" against FakeTeamServer.
  - *Run:* `scripts/he_team_two_editors.py OUTDIR [--server host:port]` starts two deployed
    Release editors (own HOME, project copy, hidden window, `HE_COLLAB_OFFLINE=1`; up in
    10 s) against `212.227.140.164:7777`. 31/31 checks, about 1 min. A creates team
    `e2e-1791962743` as Anna, adds Bob, and B signs in as Bob. Each sees the other online
    (Anna admin). A hosts invitation-only: B's uninvited join is refused (`not-allowed`
    in B's log). Then A invites, B joins (`joined`, roster Anna+Bob, snapshot marker, live
    entity). B declines a second invitation. B joins an open session without an
    invitation. B (not an admin) hosts and invites Anna, and A joins. After B signs out,
    A sees Bob offline.
  - *Witness:* the host's journal for that team, read-only over ssh:
    `created … added member Bob`, `Bob logged in`, `invited Bob`, `joins (invited)`,
    `paired`, `declined`, `opened … (open to the team)`, `joins (open)`, `paired`,
    `Bob … opened`, `invited Anna`, `Anna joins (invited)`, `paired`. Editors were stopped
    afterwards (`pgrep -x HorizonEditor` empty).
  - *Build/tests (Mac, Release, `out/build/release`):* all targets rc 0. `he_tests`
    without test_material_graph: 5078/5078 cases, 1,287,999 assertions (114 skipped = live
    cases without env), plus test_material_graph alone 102/102 (11307). Live against the host:
    relay 2/2 (79), test_net_team 8/8 (268), test_team_ui 3/3 (221),
    test_mcp_tools_collab 7/7. `editor_help_audit.py --check` rc 0.
  - *Left on the server:* teams e2e-1791962743, he-test-351180101 and uishot622389.
  - *Not verified:* a person clicking the Team panel in two visible editor windows. The
    run drives the controller below the panel, which test_team_ui covers headless.
    Windows/Linux only via CI.
