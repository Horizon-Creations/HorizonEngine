# Task board: team interop (topic 203, step 4)

Status (2026-10-15): the editor half that does not need the team stack is in:
`src/HE_Editor/TaskBoardTeam.{h,cpp}` (ImGui-free, no sockets), the Task Board
panel's Assigned To field and From the Team list, and the tests
`tests/test_taskboard_team.cpp` plus one case in `tests/test_taskboard_ui.cpp`,
both on a fake team (`tests/FakeTaskTeam.h`) and one forward-only fake clock.

**Not in yet, on purpose:** the link to the real team server. This branch forks
from `release/0.8.0`, and the whole team stack (`HE::Net::TeamClient`,
`TeamController`, `TeamView`, `FakeTeamServer`, `docs/team-server-protocol-2026-10-13.md`)
exists only on `release/0.7.0`, which is not released yet. The queen chose
(request 88): build the TeamController-free half now; the adapter, the TeamClient
op and the server change are a follow-up step once 0.7.0 is merged. Until then
`TaskBoardPanel::team()` is null in the editor and the board is local, exactly as
"no team server" is meant to look.

## What it does

- **Only with a signed-in team.** `TaskBoardPanel::setTeam(TaskBoardTeam*)`; the
  bridge's `TaskTeamLink` says `connected()`. Without one: no Assigned To field,
  no From the Team button, nothing sent, nothing queued. The edit is never
  announced later as if it were news.
- **Assigned To** (details of the selected task): a list of the team's members
  (online or "(offline)"), tick one or several, untick to take it from them. Each
  tick is one `TaskBoard::assign` through `TaskBoardPanel::edit`.
- **Who is recorded:** while signed in, `TaskBoardPanel::author()` is the member
  name (the footer's "mine" counts by it); signing out takes it back to the OS
  user name.
- **What is posted:** after every *saved* `edit()` (panel, footer list, MCP: there
  is one writer) the bridge gets the board before and after. Every new
  `TaskUpdate` written under our member name becomes a notice: Assignment →
  `assigned`, Status → `status`, Comment → `comment`. Updates that came in with a
  pull are not announced (they are somebody else's); title, priority and link
  edits are not a story and announce nothing.
- **Who hears it** (`taskNoticeRecipients`): the task's assignees, its creator,
  whoever assigned it, everyone who wrote in its history, and for an assignment
  whoever it was taken from. Never yourself. Only names on the team, in the
  team's spelling, case-insensitive, each once.
- **When:** an assignment settles for 1.5 s (`kAssignSettleMs`), so three ticks in
  the picker are one notice per person, carrying the final list (a newer
  assignment of the same task replaces any unsent one for that person, also one
  held for somebody offline). A recipient who
  is not online is held until they are; everything is held while the link is
  down. After 15 min (`kOutboxTtlMs`) a notice is dropped: the change is in the
  file by then.
- **What arrives** is a notice, **never an edit**: the bell
  (`NotificationStore`, Info, detail = comment text and project) and the panel's
  From the Team list (newest first, 20), whose row opens the task when it is on
  this board ("Not on this board yet: pull to see it." / "In project X."
  otherwise). If both ends wrote the assignment into their files, each would give
  it its own update id and the next merge would conflict. Duplicates (a reconnect
  replaying) drop on the update id; a notice from ourselves is ignored.

## Wire

### Payload (engine-defined, the server does not read it)

One JSON object, UTF-8, at most 4 KiB in practice (a title and a comment):

```json
{"type":"task-notice","version":1,"kind":"assigned","project":"Harbour",
 "task":"<32 hex>","title":"Retexture the crates","update":"<32 hex>",
 "from":"Anna","assignees":["Bob"],"time":"2026-10-15T09:12:03.120Z"}
```

`kind` is `assigned` | `status` (with `status`, the word from TaskBoard.json, and
`text`, its note) | `comment` (with `text`). An unknown kind decodes as Other and
is shown as "X updated …"; `type` other than `task-notice`, or missing ids, is
refused. `from` is overwritten by the server's word for the sender.

### Team server protocol (follow-up, CollabSession repo)

Minimal, same pattern as INVITE: a request from one connection, an event to
every connection of the target member.

- **TEAM_REQUEST op 20 NOTIFY:** `member str8 | payload str16` (u16 BE length,
  max 8192 bytes). Results: 0 ok (delivered to at least one connection),
  5 no-such-member, 11 not-online (no connection of that member: nothing is
  stored, the sender keeps it), 1 bad-request (payload too long). Not an admin op.
- **TEAM_EVENT kind 9 NOTICE:** `from str8 | payload str16`, pushed to every
  connection of `member` except the sending connection (the rule every event
  already follows). The server never parses the payload.
- A server from before op 20 answers bad-request: the client then reports
  `connected()` false for tasks (like `profilesSupported()`), so the board stays
  local against an old server instead of filling an outbox that never drains.
- Rate: the server may cap NOTIFY at, say, 30 per connection per 10 s (refuse with
  bad-request); the editor sends one per recipient per update.

Deploy is the human's (`deploy/deploy.sh`, see the topic 197 handoff); a worker
session must not run it.

### Engine follow-up (once release/0.7.0 is in this branch)

1. `HE::Net::TeamClient`: `Op::Notify = 20`, `EventKind::Notice = 9`,
   `notify(member, payload, ReplyFn)`, an `Event` with `member` = from and a
   `payload` field; `noticesSupported()` false after a bad-request on op 20.
   `tests/FakeTeamServer.h`: handle op 20 and push event 9. Test in
   `test_net_team.cpp` on the shared forward-only `g_fakeNowMs`.
2. `TeamController`: `notifyMember(member, payload)` and a `pollTaskNotice()`
   queue fed from `handleEvent` (event 9 must NOT go to the generic notice text).
3. `src/HE_Editor/TaskBoardTeamLink.{h,cpp}`: `TaskTeamLink` over TeamController:
   `connected()` = `signedIn() && noticesSupported() && !members().empty()` (an
   edit made before the roster is in would find nobody on the team to tell and
   queue nothing), `self()` = `member()`,
   `members()` from `members()` (name, online; empty between login and the
   LIST_MEMBERS reply, which the bridge reads as "not known yet" and holds the
   outbox, never as "everybody left"), `send()` = `notifyMember` (a not-online
   reply is harmless: the bridge only sends to members shown online),
   `poll()` = `pollTaskNotice()`.
4. `EditorApplication`: own a `TaskBoardTeam` and the link next to `m_team`,
   `setNotifications(&m_notifications)`, `TaskBoardPanel::setTeam(&bridge)` at
   start, `setTeam(nullptr)` before shutdown. The panel already pumps it.
5. Two-editor acceptance (step 5): Anna assigns Bob in a real editor; Bob's bell
   says "Anna assigned you …"; `scripts/he_team_two_editors.py` is the harness.
