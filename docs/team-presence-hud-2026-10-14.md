# Team presence in the editor HUD (topic 202)

Extension of the team UI from topic 197 (`docs/team-server-protocol-2026-10-13.md`) and
the saved sign-in from topic 201 (`docs/team-login-persist-2026-10-14.md`): team members
who are online but not in the collab session show up in the corner where the session's
participant icons are, set apart from them, stacked, revealed on hover (invite from there,
never remove). Team accounts get a picture and a colour like collab participants.

Plan (from the Hive): 1. profile picture and colour for team accounts (server, protocol,
editor UI to set them) — **this document's step 1**; 2. own status icon left of the bell
(signed in: own face; otherwise a red X that opens a sign-in modal); 3. presence HUD;
4. stacked icons with hover reveal + Invite; 5. the icon slides from online to in-session
on join; 6. verification.

This branch starts from topic 201's branch (merged on the queen's go-ahead, 538cc513):
both topics touch `TeamClient`, `TeamController`, `TeamView` and `FakeTeamServer.h`.

## Step 1: team profiles (colour + picture)

**Server** (CollabSession `claude/he-net-relay-server` @ b4a47a5, on top of 201's
tokens; README sections "Profile", "Datenmodell", "Requests", "Ereignisse"):

| op | name | fields | reply body |
|---|---|---|---|
| 16 | SET_COLOR | `r g b u8` (own; 0,0,0 = none) | – |
| 17 | SET_AVATAR | `size u8` (0 = remove, 1..64) + `size²×4` bytes RGBA8 (own) | – |
| 18 | LIST_PROFILES | – | `count u16` × (`name str8`, `r g b u8`, `avatar u32`) |
| 19 | GET_AVATAR | `member str8` | `size u8` + `size²×4` bytes |

Event 8 *profile* `member str8, r g b u8, avatar u32` to the team except the causing
connection (the member's other editors get it). `avatar` is 0 or the first 4 bytes of
SHA-256 over size+pixels (never 0), so a client fetches only new pictures. Team requests
may now be 17 KiB (was 1024). Storage: colour as a `profile` record in `teams.txt`
(format 3, written only while some colour is set; otherwise 2/1 as before), pictures as
`<data-dir>/avatars/<team>/<member>.rgba` (0600/0700) because `teams.txt` is rewritten on
every token login. A password change keeps the profile; removing (or re-adding) a member
removes its picture file; an unreadable picture file is skipped with a log line.

**Engine** (`HE::Net::TeamClient`): `TeamColor`, `TeamMember::{color, avatarId,
avatarSize, avatarRgba}`, `setColor/setAvatar/listProfiles`, `profilesSupported()`
(false after a bad-request to LIST_PROFILES: an old server keeps working). Profiles live
in `m_profiles` by folded name and are merged into every fresh member list; pictures are
fetched lazily, at most `kAvatarFetchesInFlight` (2) at a time, re-fetched if the id
changed while one was on its way. `EventKind::Profile` is also queued for the owner.

**Editor**: `TeamController::{self, ownColor, setColor (debounced kColorSettleMs = 400),
setAvatarFromFile (stb_image + CollabController::resampleSquareRgba → 64 px), setAvatar,
clearAvatar, useCollabProfile, decodeAvatarFile}`. `TeamView` draws "Your Profile" on
Window ▸ Team and Preferences ▸ Team when signed in: a face preview, Choose/Change
Picture... (SDL dialog in the editor, `TeamView::setPictureChooser` in tests), Remove
Picture, Use Collaboration Profile, the eight `kParticipantPalette` swatches, None, a
Custom picker. The member list got a face column (picture, else colour + initial),
drawn with ImDrawList rectangles because TeamView has no renderer. Help: `Team/Choose
Picture...`, `Change Picture...`, `Remove Picture`, `Use Collaboration Profile`, `None`,
`Custom` in `EditorHelp.cpp`.

## Handoff

- **Topic / step:** 202, step 1 of 6 (team profiles: server, protocol, editor UI).
- **Branch / commit:** `claude/team-anwesenheit-im-editor-hud-online-mitglieder-neben-colla`
  @ 421f51a8 + face fix (pushed; contains topic 201's branch @ 538cc513). Server:
  CollabSession `claude/he-net-relay-server` @ b4a47a5 (pushed, CI green:
  https://github.com/Horizon-Creations/CollabSession/actions/runs/38548406552). **Not
  deployed**: the rented host still runs an older build (no tokens, no profiles); against
  it the editor just shows "no profiles yet" (`profilesSupported()` false).
- **Done:** "Step 1" above. Code: `TeamClient.h` (TeamColor, TeamMember profile fields,
  ops 16-19, `kAvatarFetchesInFlight`), `TeamClient.cpp` (`setProfile`, `mergeProfiles`,
  `fetchAvatars`, GetAvatar branch at the top of `applyReply`), `TeamController.cpp`
  ("Your profile" at the end; colour flush in `update()`), `TeamView.cpp` (`face()`,
  `drawColourRow()`, `drawProfile()`, picture dialog), `FakeTeamServer.h` (ops 16-19,
  `setProfile()`, `noProfiles`, `writeBmp()`).
- **Verified (Mac, Release, `out/build/release`, full build of he_tests + HorizonEditor
  rc 0):** server `team_store_test` all passed, `team_test.py` 24/24 (new: test_profiles,
  test_profiles_survive_a_restart), `relay_test.py` 25/25. Engine: `test_net_team` 20/20
  (1068 assertions) and `test_team_ui` 5/5 (345), both again with
  `HE_TEST_TEAM_SERVER=127.0.0.1:47812` (local collab_server b4a47a5): 20/20 (1151) and
  5/5 (365); server log shows "set its picture (64 px)", "set its colour", "removed its
  picture". Regressions: test_collab_controller 74/74, test_mcp_tools_collab 7/7,
  test_editor_help 19/19, test_mcp_tools_settings 20/20, test_net_relay 27/27.
  `editor_help_audit.py --check` 1172/1172. Screenshot looked at:
  `team-panel-profile.bmp` (HE_UI_DUMP_DIR), profile section + faces in the roster.
- **Not verified:** the real editor window by hand (the SDL picture dialog in TeamView
  passes no parent window); Windows/Linux only through CI run
  https://github.com/Horizon-Creations/HorizonEngine/actions/runs/38558101088 (still
  running at hand-off); the rented host (not deployed).
- **Decisions:** separate ops instead of extending LIST_MEMBERS (old clients/servers keep
  working; unknown op = bad-request, unknown event = skipped). Pictures as raw 64 px
  RGBA8 (the collab avatar format, `CollabController::resampleSquareRgba`), stored as
  files beside teams.txt, colour in teams.txt (format 3 only while a colour is set). The
  team profile is its own server-side copy; "Use Collaboration Profile" copies the collab
  identity on request, nothing is uploaded on its own. Colour picks are debounced (400 ms).
  Faces drawn with ImDrawList rectangles (TeamView has no renderer).
- **Suggested, not taken:** upload the collab identity automatically when a team account
  has no profile yet; rounded clipping of the picture corners in `face()` (square pixels
  stick out of the rounded frame slightly).
- **Traps:** the fake server answered unknown ops with ok + empty body, which a client
  reads as a broken reply: every new op must be added to `FakeTeamServer.h` or all team
  tests drop their connection. The server's `members()` test helper asserts no trailing
  bytes, a reason not to append to existing replies. Deploying a format-3 teams.txt makes
  rollback to an older server impossible until all colours are None.
- **Next steps:** 1. Step 2: own status icon left of the bell — `TeamController::self()`
  + `ownColor()` give face and colour; signed-out/failed = red X opening a sign-in modal
  (reuse `TeamView` sign-in fields; `resume()` already does the automatic login).
  2. Step 3/4: HUD next to `CollabPresenceBar` (footer) — online members = `members()`
  with `online && !in session`; pictures via `CollabPresenceBar`'s `avatarTexture()` cache
  keyed by member + `avatarId`; Invite = `TeamController::invite()`. 3. Deploy b4a47a5
  (the human: `deploy/deploy.sh`), then rerun the two live cases against the host.
