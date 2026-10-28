# Team login across restarts: session tokens (topic 201)

Goal: the editor stays signed in to the team server after a restart without the
password on disk. Approach (the one the topic recommends): the team server issues a
long-lived **session token**; the editor keeps the token, not the password, and the
server can revoke it. Builds on the team server of topic 197
(`docs/team-server-protocol-2026-10-13.md`).

## Server side (step 1)

In the relay/team server, repo `Horizon-Creations/CollabSession`, branch
`claude/he-net-relay-server`, commit **363f889** (on the Mac at
`~/VSCode/EngineCollab/CollabSession`). Full spec: README section "Sitzungstokens".
Backward compatible: ops 1/2 and every existing request are unchanged, so the
deployed editors keep working against the new server.

Wire format for whoever writes the `HE::Net::TeamClient` side (step 2):

- **Log in with a token:** `TEAM_LOGIN` op **3**:
  `"HERL" | 1 | 6 | 3 | team str8 | member str8 | token str8`. The token goes in the
  password field. Answer: STATUS `logged-in` (4), or `bad-login` (23) and close for a
  wrong, revoked or expired token (the same as a wrong password, so a client cannot
  tell them apart). On bad-login the editor forgets the token and asks for the password.
- **Get a token:** request op **13** `ISSUE_TOKEN`, no fields. Reply body `token str8`,
  64 lower-case hex characters. Only on a connection that logged in with the **password**
  (op 1/2) or has changed it (SET_PASSWORD) since. On a token login the result is
  `not-allowed` (14), so a stolen token cannot mint successors.
- **Sign out on this device:** op **14** `REVOKE_TOKEN`, `token str8` (one of one's own).
  Result ok, or **17 `no-such-token`** (new result).
- **Sign out everywhere:** op **15** `REVOKE_ALL_TOKENS`, no fields; reply text "N revoked".
- **Implicit revocation:** removing the member (its tokens go with the record, and a
  member re-added under the same name gets none back), SET_PASSWORD (revokes all of the
  member's tokens), 90 days unused (`--token-lifetime-days`; every token login resets the
  count), more than 16 tokens per member (the least recently used one is dropped).
- **Live connections** that logged in with a token that became invalid get STATUS
  `bad-login` with text `session token revoked` / `session tokens revoked` /
  `password changed` and are closed. `TeamClient` already treats bad-login as
  `loginRefused()` (no reconnect), which is right here too. The connection that causes
  the revocation stays open.

Storage: `teams.txt` gets `token <team> <member> <sha256 hex> <issued> <last used>`
records (SHA-256 of the 32 raw bytes, never the token). The file says `heteams 2` once
a token exists and stays `heteams 1` while none does. A server from before tokens refuses
to start on a format-2 file, so a rollback needs all tokens revoked first.

## Editor side (step 2)

- **`HE::Net::TeamClient`** (`src/HE_Net/include/Net/TeamClient.h`): `LoginOp::Token` (3, the
  token goes in the password field), `Op::IssueToken/RevokeToken/RevokeAllTokens` (13-15),
  `Result::NoSuchToken` (17), `Reply::token`, `issueToken()`, `revokeToken()`,
  `revokeAllTokens()`, `loginOp()`, and `closeText()` (the server's text with a closing
  STATUS, e.g. "session token revoked", which `lastError()` drops for bad-login).
- **Where the token lives:** `team-session.txt` beside `config.json`
  (`GlobalState::configFilePath().parent_path()`), set in `EditorApplication::OnInit` via
  `TeamController::setTokenFile`. Five lines (`heteamtoken 1`, host, port, team, member,
  token), one sign-in only. Written to `.tmp` with `open(…, 0600)` + `fchmod` + `fsync`,
  then renamed (the McpBridge endpoint-file pattern); on Windows the per-user profile
  directory protects it, like `config.json`. Never the password, never `EditorConfig`.
  A scratch `HE_CONFIG_DIR` has no token, so automated runs stay offline.
- **`TeamController`:** after a password login (op 1/2) it sends ISSUE_TOKEN; on success
  the token goes to the file, **and the password is wiped from memory**: reconnects use the
  token from then on. `resume(server, team, member)` at start signs in with the stored token
  if it is for exactly that server (host+port), team and member (names case-insensitive).
  A server that is down at start keeps retrying (the token stays). A refused token (at start
  or pushed to a live connection) is deleted, status Failed, the message reads "The saved
  sign-in is no longer valid (…). Sign in with your password." with the server's reason only
  for a live revocation (a refused login's generic "wrong team, member or password" would
  mislead). Only the token *this* controller used is deleted, so a second editor of the
  same member on the same machine cannot erase a newer one.
- **Two ways out:** `signOut()` (Sign Out button, MCP `team_sign_out`) deletes the file and
  sends REVOKE_TOKEN on a connection that lives on in `m_leaving` until the reply (at most
  `kRevokeGraceMs` = 5 s; `TcpTransport::disconnect` drops queued bytes, so closing at once
  would lose the frame). `disconnect()` (quit, destructor, a new Sign In) keeps the token.
  `signOutEverywhere()` (new "Sign Out Everywhere" button in Window ▸ Team) sends
  REVOKE_ALL_TOKENS the same way.
- **Password change:** SET_PASSWORD revokes all tokens, including ours; the controller
  deletes the stored one and asks for a new token on the same connection (allowed after
  SET_PASSWORD, also on a token login).
- **Old server** (the deployed e3f4649 answers op 13 with bad-request): nothing is stored, the
  password stays in memory for the reconnect, exactly as in topic 197.

## Step 1 handoff

- **Topic / step:** 201, step 1 (session token protocol in the team server).
- **Branch / commit:** server CollabSession `claude/he-net-relay-server` @ 363f889, README
  note 4044869 (pushed, no PR). HorizonEngine: only this doc on
  `claude/team-login-persistieren-editor-bleibt-ber-neustarts-hinweg-a`.
- **Done:** `src/TeamStore.{h,cpp}`: `Token` in `Member`, `IssueToken`, `TokenLogin`,
  `RevokeToken`, `RevokeAllTokens`, `TokenHash`, `SetClock` (tests), format 2.
  `src/Protocol.{h,cpp}`: `TeamLoginOp::Token`, `TeamOp::IssueToken/RevokeToken/
  RevokeAllTokens`, `TeamResult::NoSuchToken`, `MakeTokenReply`.
  `src/RelayServer.{h,cpp}`: `Conn::tokenHash`, `DropTokenLogins`, login/request handling.
  `src/main.cpp`: `--token-lifetime-days`. README section "Sitzungstokens".
- **Verified (Mac, `/tmp/collab-build` RelWithDebInfo `-Werror`, plus
  `/tmp/collab-build-asan` with `-Wall -Wextra -Wpedantic -fsanitize=address,undefined`):**
  ctest 3/3 in both. team_test.py 22/22 (4 new: `test_session_tokens`,
  `test_revoking_tokens`, `test_tokens_end_with_the_member_and_the_password`,
  `test_tokens_survive_a_restart`), team_store_test "all passed" (new `TestTokens`
  covers idle expiry with a fake clock, the 16-token cap and LRU eviction;
  `TestTokenPersistence`; 4 broken token records in `TestBrokenFilesAreRefused`).
  Mutation check: with the "token login may not issue" guard and the password-change
  drop removed, 2 of the new tests failed (20/22). Then restored.
- **CI:** CollabSession run
  https://github.com/Horizon-Creations/CollabSession/actions/runs/38537985553 green for
  363f889 (the Hive does not watch that repo). 4044869 after it only adds a README note.
  **Not deployed:** the host
  212.227.140.164 still runs e3f4649. A worker may not run `deploy/deploy.sh` (the
  classifier blocks it), so the human runs it before step 3's live check.
- **Decisions:** explicit ISSUE_TOKEN request instead of a token in the `logged-in`
  text (only clients that want persistence get one). The token is presented in the
  password field of TEAM_LOGIN (same frame layout). Idle lifetime 90 days rather than a
  fixed one, so a regularly used editor never has to sign in again. No admin op to revoke
  another member's tokens (removing the member covers it). No explicit verdict from the
  human on token versus keychain is on the board yet; the queen split the plan on the
  token approach.
- **Next steps:** 1. Step 2: `TeamClient` (`src/HE_Net/src/TeamClient.cpp`) gains
  `Mode::Token` (login op 3), `issueToken(ReplyFn)` (parse `token str8`),
  `revokeToken`. `TeamController::signIn` with the password then calls ISSUE_TOKEN and
  stores the token (OS keychain or a 0600 file, never in EditorConfig as plain
  text). On start it signs in with the token, and on bad-login it forgets the token and
  shows the form. Sign Out sends REVOKE_TOKEN before closing. Extend `tests/FakeTeamServer.h`
  with ops 13-15 and login op 3. 2. The human deploys 363f889. 3. Step 3: live
  check against the host (restart keeps the login, remove-member/password change kick
  the token login).

## Step 2 handoff

- **Topic / step:** 201, step 2 (editor uses the token). Branch
  `claude/team-login-persistieren-editor-bleibt-ber-neustarts-hinweg-a`.
- **Done:** see "Editor side (step 2)" above. Code: `TeamClient.{h,cpp}` (ops, `closeText`),
  `TeamController.cpp:104` (`writeSignIn`), `:209` `resume`, `:307` `signOut`/
  `signOutEverywhere`/`leaveRevoking`, `:410` refused-token path, `:499` `requestToken`;
  `EditorApplication.cpp:1315` (`setTokenFile` + `resume`), `:12907` (`disconnect` on quit);
  `TeamView.cpp` (hint, "Sign Out Everywhere"), `EditorHelp.cpp` (Team/Password, Sign In,
  Sign Out, Sign Out Everywhere), `EditorConfig.h` comment. Fake server: login op 3,
  ops 4/13/14/15, `revokeTokensOf()`, `noTokens`, `loginOps`.
- **Verified (Mac, Release, `out/build/release`, he_tests + HorizonEditor built -j8, rc 0):**
  `he_tests --source-file=*test_net_team.cpp` 15/15 (746 assertions; 6 new cases at
  `tests/test_net_team.cpp:323-560`), `*test_team_ui.cpp` 3/3, `*test_mcp_tools_collab.cpp`
  7/7, `editor_help_audit.py --check` clean. **Against a real collab_server 363f889** run
  locally (`/tmp/collab-build/collab_server 17977 --data-dir …`):
  `HE_TEST_TEAM_SERVER=127.0.0.1:17977 he_tests … --test-case=*real collab_server*` 2/2
  (65 assertions); the server log shows issue, token login, the removed member's token
  connection refused, password change → revoke + new token, old token refused, revoke on
  Sign Out, revoked token refused. **Real editor:** config dir with
  CollabRelayAddress/TeamName/TeamMember + a token minted over the wire, hidden
  `HorizonEditor` (HE_EXIT_AFTER_FRAMES=600): log "signing in … with the saved session",
  server "logged in with a session token", token file still there (0600) after quit.
- **Not verified:** Windows (file without mode bits, `std::filesystem::rename` over an
  existing file) and Linux: only via CI. Against the host: 363f889 is **not deployed**
  (e3f4649 there answers op 13 with bad-request → the editor silently keeps the password in
  memory, covered by the `noTokens` test). Clicking through the UI by hand.
- **Decisions:** 0600 file beside config.json instead of the OS keychain: no prompt from
  macOS for an ad-hoc-signed editor after every rebuild (would also block headless runs), no
  three platform APIs, testable; the token is revocable and useless without the server,
  unlike the password. One sign-in per file. The old token is not revoked when a new
  password sign-in replaces it (LRU cap of 16 + 90 days cover it; a revoke could kick a
  second editor of the same member on the same machine). Status "Reconnecting" also shows
  while a resume waits for a server that is down.
- **Next steps:** 1. Human deploys CollabSession 363f889 (`deploy/deploy.sh`). 2. Step 3:
  `HE_TEST_TEAM_SERVER=212.227.140.164:7777 he_tests --source-file=*test_net_team.cpp
  --test-case=*session tokens through a real*` (leaves a `he-tok-N` team), then two
  real editor starts against the host (sign in, quit, start: signed in without password;
  remove member / Sign Out Everywhere from another editor kicks it). 3. CI result.

## Verification (step 3)

Code under test: 0ab82713 (`out/build/release`, rebuilt -j8, ninja: no work to do).
Server: collab_server 363f889 built from `~/VSCode/EngineCollab/CollabSession`
(`/tmp/collab-build`), run on 127.0.0.1:17991 with a scratch `--data-dir`.

- **he_tests (fake server):** `test_net_team` 15/15 (749 assertions), `test_team_ui` 3/3,
  `test_mcp_tools_collab` 7/7; `editor_help_audit.py --check` clean.
- **he_tests against the real server:** `HE_TEST_TEAM_SERVER=127.0.0.1:17991 he_tests
  --source-file=*test_net_team.cpp --test-case=*real collab_server*` 2/2 (65 assertions). Server
  log: token issued, token login, removed member's token connection "refused (bad-login):
  removed from the team by Anna", password change revokes + reissues, old token refused,
  Sign Out revokes, revoked token refused.
- **Real editor, five starts of one profile:** `scripts/he_team_token_restart.py OUTDIR
  --server 127.0.0.1:17991 --server-log <log>` → `RESULT: PASS` (35 checks). The profile's
  config.json carries CollabRelayAddress/TeamName/TeamMember as the Preferences fields
  would; `team_sign_in` (MCP, = the Sign In button's `TeamController::signIn`) is the only
  way the password goes in. Proven:
  1. password sign-in → `team-session.txt` 0600, password neither in it nor in config.json;
     SIGTERM quit (rc 0) keeps the file and the three config fields;
  2. **restart: signed in with no password** (0.1 s; editor "with the saved session",
     server "logged in with a session token"); Sign Out deletes the file, server "revoked a
     session token";
  3. negative control: without the file the next start stays `signed_out`;
  4. **live revocation:** a raw socket logs in as the same member with the password and sends
     REVOKE_ALL_TOKENS ("1 revoked") → the running editor goes `failed` with "The saved
     sign-in is no longer valid (session tokens revoked). Sign in with your password.",
     file deleted;
  5. **removed while the editor was off:** next start is refused, `failed`, file deleted,
     the token offered exactly once (no retry loop).
- **Not verified:** the host 212.227.140.164 still runs e3f4649 (`~/collab-relay/bin/VERSION`
  over ssh, 14.10. 09:50 UTC), so nothing here ran against it; the human has to run
  `deploy/deploy.sh` first (no allow rule for workers). Windows/Linux only through CI. The
  start-up notice (`notice(…, true)`) is not reachable through MCP; only its text in
  `team_status.error` is checked. Clicking the Team UI by hand.
- **Note:** MCP `team_sign_in` does not write the three config fields (only the Preferences /
  Window ▸ Team fields do), so an editor signed in only through MCP has a token file but does
  not resume after a restart. Harmless for people; the script seeds the config instead.

## Handoff

- **Topic / step:** 201, step 3 (verification). Branch
  `claude/team-login-persistieren-editor-bleibt-ber-neustarts-hinweg-a`.
- **Done:** "Verification (step 3)" above; new `scripts/he_team_token_restart.py` (one profile,
  five starts, a raw-socket second device; `--server-log` adds the server-side witnesses).
  No engine code changed in this step.
- **Verified:** everything listed above, on the Mac, against a local collab_server 363f889.
- **Not verified:** the rented host (still e3f4649), Windows, Linux (CI run 38545119057 for
  0ab82713 was still running when this was written).
- **Decisions:** verified locally instead of waiting for a deploy (queen's answer to
  request 82). The raw-socket "other device" lives in the script (stdlib, ~50 lines) rather
  than importing CollabSession's `tests/team_test.py` across repositories.
- **Pitfalls:** the editor's stdout is block-buffered into the log file: assert on editor log
  lines only after the quit. `mc.Editor` cannot restart a profile (it rewrites config.json
  and copies the project again); the script's `Profile` deletes the stale
  `mcp-endpoint.json` before each start.
- **Next steps:** 1. Human runs `~/VSCode/EngineCollab/CollabSession/deploy/deploy.sh
  horizon@212.227.140.164 7777`. 2. Then `scripts/he_team_token_restart.py OUT --server
  212.227.140.164:7777` (no `--server-log`; leaves a `tok-N` team) and
  `HE_TEST_TEAM_SERVER=212.227.140.164:7777 he_tests --source-file=*test_net_team.cpp
  --test-case=*real collab_server*`. 3. CI green → the queen merges into release/0.7.0.
