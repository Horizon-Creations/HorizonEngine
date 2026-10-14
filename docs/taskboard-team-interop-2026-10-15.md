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
5. Two-editor acceptance with two editor PROCESSES: Anna assigns Bob through
   `task_assign` in one editor, Bob's `task_notices` and bell in the other say
   "Anna assigned you …". Template: `scripts/he_taskboard_live.py` (one editor,
   the `Editor` helper of `he_rewards_live.py`) plus `he_collab_two_editors.py`
   (two at once); both editors need the same team server and two members.
   Step 5 did this in-process only (below), because there was no transport.

## MCP tools (step 5)

`src/HE_Editor/McpToolsTaskBoard.cpp`, registered in
`EditorApplication::setupMcpTools` (hooks in `McpToolRegistry.h`,
`McpTaskBoardHooks`). Every hook goes to the panel: `TaskBoardPanel::board/edit/
lastError/author/team`, so the tools use the same single copy and writer as
the panel, the footer and the team bridge.

| Tool | Mutates | What |
|---|---|---|
| `task_list` | no | Tasks (filters `status`, `assignee`, `includeDone`, `history`), `author`, `openCount`/`openMine`, `team` {connected, self, members+online, pending} |
| `task_create` | yes | `title` (+ `description`, `priority`, `status`), recorded under `author` |
| `task_update` | yes | `id` + `status` and/or `comment`; the same status again posts only the comment |
| `task_assign` | yes | `id` + `assignees` (replaces; empty unassigns). Only with a signed-in team (`no_team` otherwise, as the panel has no field), only roster names (`unknown_member`), written in the roster's spelling |
| `task_notices` | no | `TaskBoardTeam::recent()`: text (the bell's sentence), kind, from, project, task, title, status, comment, assignees |

Refusals: `no_project`, `invalid_payload`, `not_found`, `no_team`,
`unknown_member`, `save_failed` (the edit is kept in memory, as in the panel).

## Handoff

- **Topic / step:** 203, step 5 of 5 (MCP tools, two-editor acceptance, verification)
- **Branch / commit:** `claude/aufgabenboard-todo-team-interop` @ 70b67400 + the
  commit adding this section and `scripts/he_taskboard_live.py` (pushed: yes)
- **Done:** five MCP tools for the task board (above). The two-editor acceptance
  runs in-process, through the tools: Anna's editor is the real
  `TaskBoardPanel` over a project on disk; Bob's is a second board plus bridge
  in its own folder, connected over `tests/FakeTaskTeam.h`. Anna's `task_assign`
  reaches Bob's `task_notices` and bell as "Anna assigned you “Retexture the
  crates”", once. Bob's board only gets the task with the pull. Bob's
  `task_update` reaches Anna as "Bob set … to In Progress" with the comment.
- **Files touched:** `src/HE_Editor/McpToolsTaskBoard.cpp` (the tools),
  `src/HE_Editor/McpToolRegistry.h` (`McpTaskBoardHooks`),
  `src/HE_Editor/EditorApplication.cpp` (`setupMcpTools`, hooks onto the panel),
  `tests/test_mcp_tools_taskboard.cpp` (3 cases),
  `tests/CMakeLists.txt` (source, and the HE_IMGUI_ENABLED property list),
  `scripts/he_taskboard_live.py` (live editor run),
  `docs/mcp-editor-integration-plan.md` (tool table row).
- **Verified (macOS, Release, out/build/release):**
  - `cmake --build . -j8 --target he_tests` and `--target HorizonEditor`:
    both rc 0.
  - `ctest -j6`: 254/254 passed, 2 skipped (`runtime_size_app_*`), including
    `test_mcp_tools_taskboard`, `test_task_board`, `test_taskboard_team`,
    `test_taskboard_ui`, `test_taskboard_footer_ui`, `editor_help_audit` and
    `he_mcp_shim`.
  - The full `he_tests` in ONE process: 4510/4510 cases, 621344 assertions, so
    the panel's process state does not leak across files.
  - `scripts/editor_help_audit.py --check`: TOTAL 1130/1130, rc 0.
  - `scripts/he_taskboard_live.py /tmp/he203_live`: `RESULT: PASS (0
    failures)` against the deployed hidden editor. The tools are registered;
    create/update land in `Config/TaskBoard.json` under the OS user name;
    `task_assign` refuses `no_team`; `task_notices` reports not connected; a
    task written into the file by hand (a pull) appears in `task_list` and
    survives the next `task_create`.
- **Not verified:**
  - A two-PROCESS editor run with a real team server: there is no transport on
    this branch (TeamController/TeamClient are only on release/0.7.0), so in the
    real editor `TaskBoardPanel::team()` is null and `task_assign` /
    `task_notices` are only exercised in he_tests.
  - The server side (op 20 NOTIFY / event 9 NOTICE in CollabSession) and its
    deploy (the human's `deploy/deploy.sh`).
  - Windows/Linux builds: left to CI.
  - The panel's chips and the footer list clicked by hand in a real editor
    (step 2/3 open points).
- **Decisions taken:**
  - `task_assign` refuses without a team, matching the panel (queen, request 89:
    in-process acceptance, step marked done with this scope).
  - Tools take the panel's board through hooks, never a second `TaskBoard` +
    `saveTaskBoard`.
  - `task_create` and `task_update` stay usable on a local board.
- **Decisions suggested, not taken:** a `task_link` tool (link an asset/scene/
  entity by id) for a fully scripted "linked" acceptance; it needs a
  ContentManager hook.
- **Pending:** the "Engine follow-up" list above (1–4) once release/0.7.0 is
  merged into this branch, then item 5 with two processes.
- **Traps:**
  - `AppContext` has a different layout with and without `HE_IMGUI_ENABLED`
    (`EditorTab`/`tabs`). A test that builds one for `TaskBoardPanel` must be in
    tests/CMakeLists.txt's HE_IMGUI_ENABLED property list, or it does not compile
    (`no member named 'EditorTab'`).
  - The panel's board, team and clock are process state: a test that sets them
    resets all three in a guard's destructor (`setTeam(nullptr)`,
    `setTeamClock({})`, `reset()`).
  - Without `DrawTaskBoardWindow` nothing pumps Anna's bridge; call
    `TaskBoardPanel::pumpTeam()` yourself, once before the first edit, so
    `author()` is the member name.
- **Next steps:**
  1. Merge release/0.7.0 into this branch.
  2. Engine follow-up 1–4 above (TeamClient op 20/event 9, TeamController,
     `TaskBoardTeamLink`, EditorApplication wiring).
  3. Two editor processes plus the team server: extend `he_taskboard_live.py` to
     two editors signed in as two members, then `task_assign` on A and poll
     `task_notices` on B.
