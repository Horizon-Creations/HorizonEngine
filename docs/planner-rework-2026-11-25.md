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
| 5 | Assignees from the team (search), "assigned to me", sharing through the team server | done, notices not carried (server) |
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
