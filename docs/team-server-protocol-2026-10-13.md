# Team server: data model, login protocol (topic 197, server side)

Status (2026-10-13): **server step done**: teams, members (name + PBKDF2
password hash) and a login protocol live in the relay server (repo
`Horizon-Creations/CollabSession`, branch `claude/he-net-relay-server`, on
the Mac at `~/VSCode/EngineCollab/CollabSession`), persisted in
`--data-dir/teams.txt`. Presence, invitations and join settings are the next
server step; the engine side (`HE_Net` team subsystem) has not started.
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

## Handoff

- **Topic / step:** 197, step 8 (data model + login; plan step 1, third attempt)
- **Branch / commit:** CollabSession `claude/he-net-relay-server` @ ae5d379 (pushed), on top
  of f61c9f1: 6ff8f77 store+crypto, 57c4bda protocol+server, ae5d379 deploy+README.
  This doc: HE `claude/team-server-teams-mitglieder-anwesenheit`.
- **Done:** create team, log in, list/add/remove members, change password, all persisted.
- **Files touched (CollabSession):** `src/TeamStore.{h,cpp}` (model, file, rules),
  `src/Crypto.{h,cpp}` (SHA-256/HMAC/PBKDF2, no deps), `src/Protocol.{h,cpp}` (types 6-8,
  codes 4/23-26, `ParseTeamLogin`/`ParseTeamRequest`), `src/RelayServer.cpp`
  (`HandleTeamLogin`, `ProcessTeamRequests`, `HandleTeamRequest`, `DropMember`, `State::Team`
  in `Detach`), `src/main.cpp` (`--data-dir`, `--max-teams`, `--max-members`,
  `--hash-iterations`), `deploy/*` (data dir `~/collab-relay/data`), `tests/team_test.py`,
  `tests/team_store_test.cpp`.
- **Verified:** `ctest --test-dir /tmp/collab-build` 3/3 (relay_protocol 25/25 unchanged,
  team_protocol 12/12, team_store all vectors), the same with ASan/UBSan
  `-fno-sanitize-recover=all`; a deliberately wrong expectation fails (negative control);
  GitHub CI on 57c4bda green on linux-epoll, linux-epoll-sanitizers, macos-poll.
- **Not verified:** nothing deployed to 212.227.140.164 (step 5); `deploy.sh` only
  `bash -n` + unit rendered locally.
- **Decisions taken:** separate team connection with its own first frame (relay path
  untouched); hashing synchronous on the event loop, default 20000 iterations (~25 ms
  per login on the M-Mac, plain C is ~4x slower than OpenSSL's SHA instructions),
  iterations stored per member so they can rise later; strict store loading (refuse
  to start rather than drop accounts); no `--data-dir` = memory only + loud warning.
- **Decisions suggested, not taken:** a hashing worker thread (then 100k+ iterations);
  per-IP rate limit for failed logins (today: one attempt per connection); a key or
  switch that restricts who may create teams (today anyone reaching the port, capped by
  `--max-teams`); deleting a team.
- **Pending (topic 197):** presence + invite/join in the server (step 2: `m_teamConns`
  in `RelayServer.h` already tracks logged-in connections per team, add flag bit 2
  "online" to LIST_MEMBERS and push events), HE_Net client, editor UI, deploy, E2E.
- **Traps:** build in `/tmp/collab-build`, not the human's `build/`; the human's checkout
  is the working copy, don't switch its branch; `python3 -B` or `tests/__pycache__`
  appears (now in `.gitignore`); `relay_test.py` is imported by `team_test.py`.
- **Next steps:** 1. Server step 2: presence events on the team connection
  (new TEAM_EVENT type 9 server → client), `online` flag, invite (target member gets
  an event with a session id) and a per-team "others may join" setting, tests in
  `team_test.py`. 2. Deploy with `deploy/deploy.sh horizon@212.227.140.164 7777`.
