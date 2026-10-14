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

## Step 4: the stack and its menu

`TeamView::drawFooterOnline` (`TeamView.cpp:326`, now `void`) overlaps the faces: each
next face starts `round(face * 0.6)` px after the last (`onlineLayout`, `:286`, still the
one width source: `face + (shown-1)*step + "+N"`). Drawn right to left, so the leftmost
is on top and every face keeps its dot side in view; the seam is a 2 px dark ring
(`IM_COL32(0,0,0,170)`, the dot's halo colour) clipped to where it falls on the face
underneath, so the footer's background colour is never needed (the reason
`CollabPresenceBar` does not overlap). Same 4 + "+N" cap. A click pins the menu.

The menu: `drawFooterOnlineMenu(team)` (`TeamView.h:135`, `.cpp:483`), called from
`EditorUI.cpp:4316` right after `CollabPresenceBar::DrawOverlay`, built the same way
(statics written by the footer half, `s_onlineDrawn` consumed by the menu each frame;
hover keeps it open, click pins, a click elsewhere unpins). `drawOnlineMenuWindow`
(`:394`), 260 px, right edge on the stack, bottom on its top: a header ("N online in the
team[, not in this session]"), one row per member, **all** of them (face, name clipped
short of the button, dim "invited" after a press from here, kept per hosted session),
`Invite` (`probe("Invite", name)`, enabled while `hostedSession()` is set, tooltip says
why not), and `Open Team Window` (returns true → EditorUI opens Window ▸ Team). No
Remove: that stays in the Team window behind its second press. Help:
`Team/Open Team Window` (new); `Team/Invite` existed.

Found on the way: on the frame a button in the menu is held down it is ImGui's active
item, and `IsWindowHovered` without `AllowWhenBlockedByActiveItem` reads false: the menu
closed under the press and Invite never fired. `CollabPresenceBar::drawMenuWindow` uses
the same flags without it (its Remove/Block buttons, unpinned): not changed here, not
tested.

Test `test_team_ui.cpp:822`: 6 online (Anna admin): 4 faces overlapping in roster order,
Bob fully his blue on top, no menu unhovered; hover → 6 Invites, all disabled, 0
Remove/Confirm Remove, menu above the stack; mouse onto the menu keeps it, away closes;
`team.hostSession` → Invite Eli enabled, click → op 10 at the fake, Eli invited, menu
still open; click on the stack pins, a click elsewhere unpins; sign-out removes both.
Step-3 test: click now pins, `Open Team Window` asks for the window. Screenshots
`team-footer-online-stack`, `-menu`, `-menu-hosting`.

## Step 5: the slide when someone joins

A member who leaves the stack because they joined the session now travels: their
face slides from where it stood in the stack (from the "+N" if it was not shown) to
their face in the session's cluster, 0.4 s ease-out (`TeamView::kJoinSlideSeconds`),
turning from the team's rounded square into a circle on the way, green dot fading.
The session cluster leaves that face out until the slide lands. Joining only:
someone who leaves the session reappears in the stack at once.

- **Detection** `stepJoins` (`TeamView.cpp:359`), once per ImGui frame from whichever
  footer call comes first: last frame's stack positions (`s_stackLast`, recorded by
  `drawFooterOnline` `:452/:460`) against this frame's `onlineOutsideSession()`. A
  name that left the stack while its member is still online and not you is "in the
  session" by step 3's rule; gone offline gets no slide. Slides hold name, start
  rect and start time only; member and target are looked up by name every frame.
  Dropped when done, when the member is back in the stack, or on sign-out.
- **Hide query** `footerJoining(team, name)` (`TeamView.h:159`, `.cpp:600`), called by
  `CollabPresenceBar::DrawFooter` (`CollabPresenceBar.cpp:357`) for every shown face
  before it draws it. It runs the step itself, so the face is already left out on the
  join frame (the cluster is drawn before the stack in EditorUI's chain).
- **Target** `CollabPresenceBar::FooterFaces()` (`.h:46`): every shown face's rect by
  participant name, skipped ones included. Matched in any case
  (`TeamController::sameMemberName`, the rule's own comparison, now public). No
  face to land on (session "+N", no cluster) → the slide is dropped, not drawn.
- **Ghost** `drawFooterJoins` (`TeamView.cpp:608`), called from `EditorUI.cpp:3439`
  after the online block, outside its `onlineW > 0` guard (the stack is gone when the
  last one online joined). Footer draw list only, no width: the chain is unchanged.
  `faceAt` gained a `roundness` (default 0.2, unchanged for every other caller).

Test `test_team_ui.cpp:990`: the harness footer got a session stand-in (grey circles,
`Ui::session`, hides via `footerJoining`, reports `sessionFaces`). Bob joins: on the
join frame he is out of the stack, the ghost is at his old stack rect, his session
face hidden; frame 6 strictly between, blue pixels in the ghost; frame 22 within
1 px of the target, still hidden; frame 26 ghost gone, session face at the target.
Cleo joins as "cleo" (any case) and leaves: back in the stack, nothing sliding.
No session cluster: the slide is dropped. Shots `team-footer-joining`, `-joined`.

Not done: the other faces jump when one leaves the stack (the stack re-lays out and
the session cluster grows by a face; both right-anchored in the chain). A picture
face keeps square cells while its frame rounds (cells are rectangles, ImGui cannot
clip round); it lasts 0.4 s and the real round face replaces it.

## Step 6: the footer in real editors

Steps 3-5 were only ever seen in the `test_team_ui` harness (CollabPresenceBar is compiled
without ImGui there). `HE_DUMP_LIVE` and the MCP screenshot tools picture the scene only
(`CaptureViewport` / `RenderSceneImage`), so the UI first needed its own capture:

- **`src/HE_Editor/EditorUiShot.{h,cpp}`** (new). `HE::Ed::UiShot::captureDrawData(path,
  bandRows)` is the tutorial witness's CPU capture moved out of `TutorialPanel.cpp` (which
  now calls it): a clone of this frame's draw data through `tests/ImGuiSoftwareRaster`.
  `liveAfterRender()` (called in `EditorUI.cpp` right after `ImGui::Render()`):
  `HE_DUMP_UI=<dir>` + `HE_DUMP_UI_TRIGGER=<file>`, trigger line
  `<name> [frames] [x y] [band=N]` → `<dir>/<name>-NNN.bmp` for `frames` frames, the mouse
  at x y for the burst (negative = away), only the bottom N rows with `band=`. The trigger
  is removed after the last write. A whole 1600x900 frame costs ~0.7 s on a loaded Mac, which
  swallowed the 0.4 s slide in one frame gap (first run); the 60-row footer band keeps up.
- **`scripts/he_team_presence_shots.py`** (new): a local `collab_server` (b4a47a5, rebuilt in
  `/tmp/collab-build`), hidden editors Anna (A, creates the team) and Bob (B), and Cleo, Dan,
  Eli, Fay online over `team_test.py`'s raw sockets (from the CollabSession checkout) with a
  colour each, Dan with a striped 64 px picture; Anna's and Bob's colours are set the same
  way on a second connection. Checks are colour boxes in the footer pixels.

**Result: `RESULT: PASS (0 failures)`** (`team-presence-hud-2026-10-14/run.log`). Pictures in
`docs/team-presence-hud-2026-10-14/`:

- `footer-a-online.png`: B, C, Dan's picture, E stacked + "+1" (Fay hidden), Anna's own blue
  face right of the bell. `footer-b-online.png`: Bob's side, Anna in his stack.
- `menu-no-session.png` / `menu-hosting.png`: the hover menu (mouse put on the stack) over
  the docked Content Browser, one row per member incl. Fay, Invite disabled without a
  session and enabled while A hosts, no Remove, `Open Team Window`.
- `footer-a-hosting.png`: stack | rule | A's session face.
- `slide-strip.png`: frames 12-18 of the join burst (2x). Bob's colour stays at x 1306 until
  frame 12, then 1338, 1358, 1375, 1384, 1386 (ease-out), round on the way, over the rule and
  A's face, then lands. `footer-a-joined.png`: C, D, E, F stacked, session A + B;
  `footer-b-joined.png`: Anna gone from Bob's stack into his session cluster.
- `footer-a-left.png`: Bob leaves the session → back in A's stack at once (no reverse slide).
- **Green dot vs. the footer's edge** (open since step 3), measured in `a-online`: the footer
  is rows 876-899 (24 px), the dots rows 886-893. Not clipped under the editor font.

**Findings (not fixed, for the queen):**
1. The slide changes colour where it lands: the ghost is Bob's **team** colour (purple), but
   the session cluster draws participants with their **collab identity** colour (Bob's red
   ring), and Anna's own team face (blue) and her session face (pink) differ as well.
   They agree only after "use the collab profile" (`TeamController::useCollabProfile`).
   Either the session faces of team members take the team profile, or the slide fades into
   the target's colour.
2. The other stack faces jump when one leaves (frame 13: C takes slot 1 at once). Known since
   step 5.
3. `CollabPresenceBar::drawMenuWindow` lacks `ImGuiHoveredFlags_AllowWhenBlockedByActiveItem`
   (step 4's note), not exercised here: the session menu was not opened.

## Handoff

- **Topic / step:** 202, step 6 of 6 (verification). Server CollabSession b4a47a5 still
  **not deployed** (step 1); the rented host lacks ops 16-19, so profiles run locally only.
- **Done:** "Step 6" above. Code: `EditorUiShot.{h,cpp}`, `TutorialPanel.cpp` (uses it),
  `EditorUI.cpp` (hook), `src/HE_Editor/CMakeLists.txt`; `scripts/he_team_presence_shots.py`.
- **Verified (Mac, Release, `out/build/release`):** HorizonEditor -j8 rc 0; two hidden real
  editors + local server: PASS, every check above, pictures looked at. No editor or server
  left running.
- **Tests:** he_tests -j8 rc 0; `test_team_ui` 10/10 (672 assertions), `test_net_team` 20/20 (1068);
  full run without test_material_graph 5112/5117 cases, 1289587/1289602 assertions. The 5
  red cases are all `test_heproj_registration.cpp` (Linux XDG assets not found): this branch
  lacks release/0.7.0's ae9cf6ed (find `scripts/linux_assets` by walking up; ccache makes
  `__FILE__` relative), so they go green on merge, not caused here. `editor_help_audit.py
  --check` 1173/1173. CI https://github.com/Horizon-Creations/HorizonEngine/actions/runs/38589034072
  started at hand-off.
- **Not verified:** the tour witness (`HE_DUMP_TUTORIALUI`) after the move was not rerun
  (same code, now in EditorUiShot.cpp); Windows/Linux only via CI; the session menu.
- **Decisions:** UI capture by trigger file in the hidden editor, never a visible window;
  members without an editor from raw sockets (no new MCP tool); footer band for bursts.
- **Traps:** no Pillow on this Mac (the script reads BMPs itself, `sips` converts); a CMake
  re-run in this tree rebuilt 1112 steps (start long builds in the background and poll);
  full-frame UI shots are ~0.7 s each, so animations need `band=`.
- **Next steps:** 1. Queen: decide finding 1 (team vs. collab colour of session faces).
  2. Deploy b4a47a5 to the rented host (the human). 3. Optional: ease the stack's jump.
