# Team login across restarts: session tokens (topic 201)

Goal: the editor stays signed in to the team server after a restart without the
password on disk. Approach (the topic's recommended one): the team server issues a
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

## Handoff

- **Topic / step:** 201, step 1 (session token protocol in the team server).
- **Branch / commit:** server CollabSession `claude/he-net-relay-server` @ 363f889
  (pushed, no PR). HorizonEngine: only this doc on
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
- **Not verified:** CollabSession CI run
  https://github.com/Horizon-Creations/CollabSession/actions/runs/38537985553 was
  queued at handoff (the Hive does not watch that repo). **Not deployed:** the host
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
