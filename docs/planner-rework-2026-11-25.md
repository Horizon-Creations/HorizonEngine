# Planner rework (2026-11-25)

What the user asked for, point by point, and where it stands. Earlier history of the
Planner: `docs/planner-plan-2026-10-22.md` (its handoffs are still valid for what is not
named here).

| # | Request | State |
|---|---|---|
| 1 | Pages: fluid writing, Markdown as you type, code blocks with highlighting | done |
| 1 | Export a page as .md / .docx | done (both) |
| 2 | Sidebar sections set apart | done |
| 3 | Popups never past the window edge (task details ran behind the dock) | done |
| 4 | Tasks toolbar grouped, board uses the width, "By status" shows as "Tasks" | done |
| 5 | Assignees from the team (search), "assigned to me", sharing through the team server | done; notices carried since step 2 (server op 21, needs the deploy) |
| 5b | Planner files live in a collaboration session | done (step 2, below) |
| 6 | Calendar + timeline as one view | done (Timeline / Month toggle, one sidebar section) |
| 7 | macOS Calendar | done as .ics + "Add to Calendar" (opens in Calendar); no EventKit |

## Handoff

- **Topic / step:** Planner rework, all 8 steps of the request
- **Branch / commit:** `release/0.8.0`, commits 9dbeae497 .. (this doc's commit), pushed
- **Done:** see the table. In user terms: the task details flyover and all planner popups stay inside the
  editor window (they scroll); the Tasks board fills the width, its toolbar is New/Search/Mine on the
  left and Group/Filter/Properties on the right; pages turn `# `, `- `, `1. `, `[] `, `> `, `---`, three
  backticks into blocks the moment they are typed, Enter splits at the cursor, Backspace at a block's
  start un-formats or joins; code blocks colour C++/C/C#/Python/Lua/JSON/SQL/GLSL/HLSL/AngelScript;
  pages export to .md/.docx; timelines and calendars are one thing with a Timeline/Month switch and
  export .ics; with a team signed in, any Person cell picks from the roster with a search; planner and
  task board saves trigger the team-server sync.
- **Files touched:**
  - `src/HE_Editor/PlannerPanel.cpp`: `keepInside`/`beginPopup` (~163-190, every planner popup goes
    through them), flyover clamp in `drawDatabase` (search `keepInside(ImVec2(wmin`), `toolbarRight`
    (~3506), `segmented` (~3437), sidebar `sidebarSection`/`sidebarEmpty` (~211), `drawBoard` stretch
    lanes (~1340), `commitBlockText`/`backspaceAtStart` (~2180), `codeBlock` (~2694), `exportPage`
    (~2999), `exportCalendar` (~3467), `assigneesCell` search (~540), Mine filter (search `s_mineOnly`).
  - `src/HE_Editor/PlannerExport.{h,cpp}` (new): stored-zip writer + CRC-32, `pageToDocx`, `eventsToIcs`.
  - `src/HE_Editor/TaskBoardTeamLink.h` (new): `TaskTeamLink` over `TeamController`; wired in
    `EditorApplication.cpp` next to `m_teamSync.setNotifications` (bridge, saved hooks) and unwired
    before `m_team.disconnect()`.
  - `PlannerWorkspace.{h,cpp}` `setSavedHook` (every file write), `TaskBoardPanel` `setSavedHook`.
  - `PlannerImport.cpp` `ensureTasksSchema`: board named "Tasks", id still `derivedId("view","By status")`.
  - `Planner.cpp` `query`: Person filters compare in any case.
  - `vendor/ImGuiColorTextEdit`: `HE-PATCH(scroll-past-end)` (`SetScrollPastEndEnabled`).
- **Verified:** full `he_tests` in one process 6404/6404 cases (2 467 511 assertions) after the team commit;
  after the review fixes `--test-case="planner*,*help*"` 146/146; `editor_help_audit.py --check` clean;
  `make HorizonEditor` rc 0 (macOS, Release, cmake-build-release). Headless shots looked at: details flyover inside the panel with a
  scrollbar, board lanes over the full width, grouped toolbar, sidebar sections, Month view with the
  toggle, a page with a coloured C++ block, the team picker narrowed by "bo", the Mine filter. The
  .docx: `textutil -convert txt`, `unzip -t`, every part parsed as XML; pPr children in schema order
  (tested). The .ics read by eye (CRLF, exclusive DTEND, UID) and by test_planner_export.cpp. An old
  Tasks views file with the board named "By status" reloads as "Tasks" (test).
- **Not verified:** the real editor with a mouse/trackpad (typing feel, IME in code blocks: the
  ScriptEditorPanel workaround is copied, not tried); Word itself opening the .docx (only textutil);
  Calendar.app importing the .ics (opening it would add events to the user's calendar); a real
  team server (the link is tested against `FakeTeamServer`); two editors syncing planner files through
  team-server mode; Windows/Linux builds (left to CI).
- **Sharing, plainly:** planner and task board files go to the team only when the project's
  `sourceControl.mode` is `teamServer` (TeamSyncController, debounced after each save); otherwise they
  travel with git as before. Not tried against a real server. Task notices (the bell) are not carried.
- **Decisions taken:** EventKit not used (needs an entitlement + permission prompt and an Obj-C++ TU;
  .ics through the system is enough to "add to Calendar"). Timeline and calendar stay two ViewKinds
  in the file; the toggle flips `View::kind`, so old files and MCP tools are unchanged. Notices: the
  link drops them as sent (no outbox build-up).
- **Decisions suggested, not taken:** a NOTIFY request on the team server (CollabSession repo) so task
  notices reach teammates; then `TeamControllerTaskLink::send/poll` get real bodies.
- **Pending:** 1. server op for task notices + two-editor acceptance (`docs/taskboard-team-interop-2026-10-15.md`
  "Engine follow-up"; op 20 is taken by SC_TICKET now, pick a free number). 2. Live planner sync inside a
  collab session (today: team-server *source control* sync on save, i.e. after the debounce, not live).
- **Traps:**
  - An ImGui text field cleared with `ClearActiveID()` and focused again the next frames no longer
    receives Enter (key routing): keep the field active and call `ReloadUserBufAndMoveToEnd()` instead.
  - A dated view made with "+" is named "<data> timeline" even after switching to Month.
  - Code blocks write their text 0.8 s after typing pauses (trailing, by the editor's undo index), and
    when left; every write is a save the team sync hears.
  - The Tasks views file only exists once a Tasks view changed; the rename to "Tasks" happens in memory
    on every load (`ensureTasksSchema` from `Workspace::afterLoad`).
  - he_tests now compiles TextEditor.cpp/LanguageDefinitions.cpp (tests/CMakeLists.txt).
- **Next steps:** 1. Try it in the real editor (type in a page, a code block, the flyover near the
  bottom). 2. Server NOTIFY op. 3. EventKit only if a two-way calendar sync is wanted.

## Handoff (step 2: task notices over the team server, planner files live in a session)

- **Branch / commit:** HorizonEngine `release/0.8.0` (this commit), CollabSession
  `claude/he-net-relay-server` @ 57774f2 (both pushed). **Server not deployed** (the human's).
- **Done:**
  - Server: TEAM_REQUEST **op 21 NOTIFY** (`member str8 | payload str16`, ≤ 8192 bytes) → TEAM_EVENT
    **kind 10 NOTICE** (`from str8 | payload str16`) to every connection of that member except the
    sender's; `no-such-member` 5, `not-online` 11, too long = bad-request; payload never logged or kept.
    `CollabSession/src/RelayServer.cpp` (case `TeamOp::Notify`), `Protocol.{h,cpp}`, README tables.
  - Engine: `TeamClient::notify` + `EventKind::Notice` + `noticesSupported()` (false after an old
    server's bad-request); `TeamController::sendNotice/pollNotice` (queue capped at 256);
    `TaskBoardTeamLink.h` sends/polls for real (old server: drops as sent). The bell / "From the
    Team" list is the existing `TaskBoardTeam` path.
  - Live sync: Planner/Task Board files go through the collab asset channel under the key
    `::Config::<path under Config>` (`CollabController::kConfigKeyPrefix`, `isConfigSyncKey`: only
    `Planner/**.json` and `TaskBoard.json`). `EditorApplication::plannerFileSaved` publishes on every
    save (and notes the team sync); `publishAsset` gives the lock back right after (no tab holds a
    planner file); `collabLocalPath` maps and re-checks the key; the receiver writes the file and the
    Planner's own disk poll (Workspace::poll, TaskBoardPanel) takes it in, as after a pull.
- **Verified:** CollabSession `ctest` 7/7 (team_test.py 25/25 incl. `test_notices`); he_tests
  `Live sync*,Team*,planner*,*help*,*task*,*Collab*,*team*` 411/411; `Team: a task notice through a
  real collab_server` against the freshly built server on 127.0.0.1 (Anna → Bob, payload intact, log
  without payload); collab test: a page saved on the host, then by the client, then the host again,
  arrives each time and no lock stays held; HorizonEditor build.
- **Not verified:** the deployed server (old binary until deployed: notices are then dropped as sent,
  everything else works); two real editor processes in a session editing the same page at the same
  moment (the second save inside one round trip is turned away by the brief lock and goes out with
  that editor's next save); a joiner does not get the host's planner files at join time (only saves
  made during the session; the rest comes with source control).
- **Next steps:** 1. Deploy the server (below). 2. Two-editor check with real processes. 3. If wanted:
  send the planner folder to a joiner with the session snapshot; a rate cap on NOTIFY.

### Deploying the team server

From `~/VSCode/EngineCollab/CollabSession` on branch `claude/he-net-relay-server` (HEAD 57774f2):

```
ssh-add --apple-use-keychain ~/.ssh/id_ed25519
deploy/deploy.sh horizon@212.227.140.164 7777
```

It sends the committed HEAD as `git archive`, builds on the server, swaps the binary and restarts
the user service `collab-relay`. Teams, passwords and projects in `~/collab-relay/data` stay. Open
collaboration sessions and team connections drop once; editors reconnect by themselves. Same port,
so no firewall change. Check: `ssh horizon@212.227.140.164 cat collab-relay/bin/VERSION` says
`57774f2`, and `journalctl --user -u collab-relay -n 20` shows the restart.
