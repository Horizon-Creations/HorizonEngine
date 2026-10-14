# Team presence in the editor HUD (topic 202)

Extension of the team UI from topic 197 (`docs/team-server-protocol-2026-10-13.md`) and
the saved sign-in from topic 201 (`docs/team-login-persist-2026-10-14.md`): team members
who are online but not in the collab session show up in the corner where the session's
participant icons are, set apart from them, stacked, revealed on hover (invite from there,
never remove). Team accounts get a picture and a colour like collab participants.

Plan (from the Hive): 1. profile picture and colour for team accounts (server, protocol,
editor UI to set them) — **this document's step 1**; 2. own status icon right of the bell
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

## Step 2: your own status right of the bell

`TeamView::footerStatusWidth()` / `drawFooterStatus(team, settings, form)`
(`TeamView.h:92-105`, `TeamView.cpp:189`): one text-line square (the bell's slot height).
Signed in: your face (`faceAt`, `TeamView.cpp:290`, the draw-list half of `face()`), a
click returns true and EditorUI opens Window ▸ Team. Otherwise two strokes: red
(`Theme::Bad`) for signed out / refused / reconnecting, amber while a first sign-in is
under way; a click opens the "Team Sign-In" modal (`drawSignInModal`, `TeamView.cpp:464`):
member + password (the existing `Team/Member`, `Team/Password`, `Team/Sign In` help
entries), server and team only when one was empty as it opened, the status line with the
refusal, Sign In or Enter calls `TeamController::signIn` (works from Failed and
Reconnecting: `begin()` disconnects first), Cancel. The modal stays open on a refusal and
closes itself once signed in. Hover tooltip per state. In `EditorUI.cpp:3373-3391` the
slot sits between FPS and the bell; `teamSlot` (width + 10 px) is subtracted in every block
to its left. Test: `test_team_ui.cpp:571`.

## Handoff

- **Topic / step:** 202, step 2 of 6 (own status icon + sign-in modal).
- **Branch / commit:** `claude/team-anwesenheit-im-editor-hud-online-mitglieder-neben-colla`
  @ 5a5caae1 (+ this doc). Server CollabSession b4a47a5 is still **not deployed** (step 1).
- **Done:** "Step 2" above.
- **Verified (Mac, Release, `out/build/release`, he_tests + HorizonEditor built -j8, rc 0):**
  `test_team_ui` 6/6 (433 assertions; new case: X is red in pixels, modal opens, wrong
  password refused and shown, Enter signs in, modal closes, face is blue in pixels with no
  red, face click asks for the Team window, Cancel, empty team asks for team+server and
  keeps the rows while typing). Screenshots looked at: `team-footer-signed-out`,
  `-modal-refused`, `-signed-in`, `-modal-ask-team` (HE_UI_DUMP_DIR). `test_net_team` 20/20,
  `test_editor_help` 19/19, `test_collab_controller` 74/74, `test_mcp_tools_collab` 7/7,
  `editor_help_audit.py --check` 1172/1172. Hidden `HorizonEditor` 120 frames with a scratch
  HE_CONFIG_DIR: clean exit, 0 errors (a smoke run; whether the footer was on screen
  without a project is not shown by the log). Full he_tests: see the result post.
- **Not verified:** the real editor window by hand (footer placement next to the real bell,
  the modal over docked panels); Windows/Linux only through CI.
- **Decisions:** placement *right* of the bell as the board says (step 1's handoff said
  left). Connecting = amber X, Reconnecting = red X (its modal can retry at once with the
  password). The modal lives in TeamView (no AppContext) so tests draw it; server/team rows
  decided once at open (else they vanish with the first letter). Esc is not wired to
  Cancel (ImGui modals ignore it; not tested, so not claimed).
- **Suggested, not taken:** a dim grey X (or none) when no team server is configured at
  all: every editor without a team now shows a red X in the footer.
- **Traps:** the footer chain in EditorUI is hand-maintained: a new slot must be
  subtracted in every block to its left. `face()` calls Dummy; in a fixed slot use
  `faceAt`. In tests the footer surface clears `items` itself (drawSettings/drawPanel
  clear the probe, drawFooterStatus does not).
- **Next steps:** 1. Step 3/4: presence HUD next to `CollabPresenceBar` — online members
  = `members()` with `online && !in session`; faces via `faceAt` or `CollabPresenceBar`'s
  texture cache keyed by member + `avatarId`; Invite = `TeamController::invite()`.
  2. Deploy b4a47a5 (the human, `deploy/deploy.sh`), then the live cases against the host.
