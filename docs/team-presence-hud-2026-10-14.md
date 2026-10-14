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

## Step 3: who else in the team is online, beside the session's faces

`TeamController::onlineOutsideSession()` (`TeamController.h:153`, `.cpp:699`): members
with `online && !you`, minus every member a participant of the running collab session
(`m_collab.inSession()`, team session or not) is named after, case-insensitive
(`sameName`). Empty while not signed in. The rule alone is `onlineOutside(members,
names)`, static. Name matching works because a team session's participants carry their
member name (`hostSession`/`join` pass `member()` as display name; `applyLocalIdentity`
sets picture/colour/key, not the name).

`TeamView::footerOnlineWidth(team)` / `drawFooterOnline(team, sessionBeside)`
(`TeamView.h:109-123`, `TeamView.cpp:262-356`): one `##teamOnline` hit box, up to 4
faces + "+N" (layout shared by width and draw, like `CollabPresenceBar::layoutFor`), face
= `footerStatusWidth()` square via `faceAt`, each with a green `Theme::Ok` dot in the
corner; a 1 px `TextDim` rule in the middle of the 16 px gap to its right while
`sessionBeside`. Hover: tooltip with the names ("not in this session" when one runs);
click returns true, EditorUI opens Window ▸ Team. Each face is probed as
`##teamOnlineFace` with the member as row (step 4's Invite test can target them).
`EditorUI.cpp:3420-3433`: the slot sits directly left of `CollabPresenceBar` (online
left, in-session right, so step 5's slide runs towards the bell); `onlineW` + its 16 px
gap is subtracted in the sync, activity and MCP blocks.

Tests (`test_team_ui.cpp:688`, `:708`): the rule (you, offline, any-case match, a
non-member guest); the footer signed out (no slot), signed in (Bob blue + Cleo with green
dot, not Anna, not offline Dana, left of the status), click → Team window, a real
loopback collab session (`startHosting` + a second `CollabController` joining as "Bob")
takes Bob out, his leaving brings him back, everyone in the session (Cleo joins as
"cleo") drops the slot to width 0, sign-out too. Screenshots `team-footer-online`,
`-online-bob-in-session`.

## Handoff

- **Topic / step:** 202, step 3 of 6 (online team members in the footer corner).
- **Branch:** `claude/team-anwesenheit-im-editor-hud-online-mitglieder-neben-colla`
  (step-3 commit + this doc). Server CollabSession b4a47a5 still **not deployed** (step 1).
- **Done:** "Step 3" above.
- **Verified (Mac, Release, `out/build/release`, he_tests + HorizonEditor -j8, rc 0):**
  `test_team_ui` 8/8 (497 assertions), screenshots looked at (crop of the footer: B blue
  and C grey squares with green dots left of Anna's face; with Bob in session only C, and
  the rule). `test_net_team` 20/20, `test_collab_controller` 74/74, `test_editor_help`
  19/19, `test_mcp_tools_collab` 7/7, `editor_help_audit.py --check` 1172/1172 (`##`
  labels need no entry). Hidden `HorizonEditor` 120 frames, scratch HE_CONFIG_DIR: clean
  exit, 0 errors (smoke only). Full he_tests without test_material_graph: 5115/5115
  cases, 1289388 assertions, rc 0.
- **Not verified:** the real editor window by hand (the cluster next to the real
  CollabPresenceBar and bell, the rule between them: the test harness draws only status +
  online cluster, so its rule stands next to your own face); Windows/Linux only via CI.
- **Decisions:** the cluster shows whenever signed in, with or without a session (the
  board: online members who are not in the session). Name match without a
  `teamSession()` gate: a plain session with a participant of a member's name counts as
  "in the session" (needed for the loopback test, harmless otherwise). Separation =
  square faces + green dot vs. the session's ring circles, plus the rule. Same 4 + "+N"
  cap as the session cluster. Click opens the Team window (like your own face).
- **Traps:** `onlineOutsideSession()` returns pointers into `members()`: use them in the
  same frame only. The test footer has no bell/session cluster; `sessionBeside` there is
  `team.sessionRunning()`.
- **Next steps:** 1. Step 4: replace the side-by-side faces in `drawFooterOnline` with an
  overlapped stack, hover reveal (a popup like `CollabPresenceBar::DrawOverlay`, drawn
  from the overlay pass) listing each member with Invite (`TeamController::invite`, only
  while hosting, like the Team window), never Remove; the layout helper `onlineLayout` is
  the one place to change the width. 2. Step 5: the slide, keyed by member name between
  this cluster and `CollabPresenceBar`. 3. Deploy b4a47a5 (the human).
