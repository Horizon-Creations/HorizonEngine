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

(filled in at the end of the step, below)
