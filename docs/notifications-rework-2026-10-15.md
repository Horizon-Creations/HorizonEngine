# Notification rework — three tiers, popups, buttons (2026-10-15)

## What it is now

`NotificationStore` (`src/HE_Editor/NotificationStore.h`) is the one channel. Every
notification has a tier that decides how loud the editor is about it; all tiers are
also kept in the notification centre (footer bell + flyout).

| Tier | Where it shows | Lifetime | Use it for |
|---|---|---|---|
| `Info` | notification centre only | stays | something worked / someone else did something; nothing to do |
| `Important` | toast bottom-right, takes no focus, hover keeps it, ✕ closes it | 8 s (20 s with buttons) | did not fully work, or wants an answer soon, user not blocked |
| `Critical` | modal dialog, one at a time (FIFO), waits for any dialog already open | until acknowledged | what the user just did did not happen, or work is at risk |

Posting: `NoteSpec` (level, title, text, detail, assetPath, `key`, `actions`,
`timeoutMs`, `popup`) via `store.post(spec)` / `HE::Ed::notify(spec)`; the old
`notify(level, text, detail, path)` form still works. `NoteAction{label, run, primary,
dismisses}`: `run` executes on the main thread. `key`: a new post replaces the old one,
`resolve(key)` withdraws it (buttons gone, toast/modal closed, row kept as a record).
`popup=false` = centre only whatever the level (the engine log bridge uses it for errors).

Collapsing: identical consecutive plain posts still merge (`x3`); rows with buttons or a
key never merge.

## Where the pieces are

- Store + all popup policy (toast clock, modal queue, what a click does): `NotificationStore.{h,cpp}` — ImGui-free, tested without a frame.
- Drawing: `NotificationPopups.{h,cpp}` (`DrawOverlay`, `DrawActions`), called last in `EditorUI.cpp` after `NotificationBar::DrawOverlay`. Centre rows (`NotificationBar.cpp`) draw the same buttons, or "Answered: Join" / "No longer needed" once handled.
- Levels were `Info/Warning/Problem`; now `Info/Important/Critical`. Old `Warning` and `Problem` both became `Important`, then these were raised on purpose: project-file saves (ProjectSettings, AudioMixer, Export app settings), "no EngineContent catalogue", "entity could not be sent", "no game-logic SDK", scene save/open failure, play-restore failure, collab session failures, team sign-in/host/join failures.

## What was wired (the user-visible changes)

- **Collab invitation** (`TeamController.cpp`, `handleEvent` Invited): Important toast "Join" (primary) / "Ignore" (→ decline), keyed by session; withdrawn when the session closes or the invite is answered in the Team window.
- **Collab join/host failure, dropped link, kicked/banned** (`CollabController::announceFailure`, called at the top of `update()`): one Critical modal per failure with "Rejoin" / "Try again" where it makes sense; withdrawn when the failure ends. The red line in the Collab panel is unchanged.
- **Team**: sign-in refused, host refused, join failed (with "Try again") are Critical; connection lost is one keyed Important that resolves on reconnect.
- **Scene**: save failure = Critical with "Retry" (`saveSceneToPath`); open failure = Critical; **fixed a data-loss path** — the "Save" in the unsaved-changes dialog used to run quit/switch-project even when the scene write failed (`EditorUI.cpp`, `doSaveScene` now returns the result).
- **Autosave** (scene + asset) failures: Important once per outage (`reportAutosaveFailure`).
- **Git**: errors are one keyed Important ("Source control"); success text ("Fetched: …", "Pushed") now also goes to the centre as Info.
- **Content Browser**: create/delete/rename/revert/import/reimport/C++-class failures were log-only; now an Important toast with the reason and path.
- **MCP bridge**: start failures raise an Important.

## Not done (candidates, from the gap survey; all verified by reading in the survey, none changed)

1. Export / game-logic build: result lives only inside the Build modal (`BuildProgressDialog`); nothing if it is closed (`ExportDialogPanel.cpp:1484,1497,1974`). `GameLogicBuildPanel.cpp:184` duplicates the Build modal.
2. EngineContent sync failures are `HE_LOG_WARN` only inside `HE_ContentSync` (`EngineContentSync.cpp:108,124,141,200,291`); `:124` means working on a stale manifest silently. Needs a callback or error struct from the library, which cannot see the store.
3. Git: classify errors (auth / git missing → Critical, push rejected → Important with "Open Source Control"), pull-with-conflicts has no notification (`SourceControlPanel.cpp:1447-1460`), `GitService.cpp:432` LFS install failure is log-only.
4. Team sync: `TeamSyncService.cpp:744,960` log-only.
5. "Open Collaboration" / "Open Preferences" buttons need a hook from `EditorApplication` to the UI's `revealFloatingWindow` (static in `EditorUI.cpp`); the large-assets refusal has no button for that reason (the panel's own dialog answers it).
6. Peer joined/left Info notes: written, then removed — they made the host's store non-empty in `test_collab_controller.cpp` cases that assert "nothing was posted". Decide whether those tests should tolerate Info.
7. A "Do not disturb / no popups" editor setting (store would simply treat `popup` as false).
8. Several `CollabController` notes still carry comments saying "Warning, not Problem" — the code is `Important` now; the comments are stale.

## Handoff

- **Topic / step:** editor notifications, step 1 (tiers, popups, buttons, first wiring)
- **Branch / commit:** claude/notification-system-rework-g84g70 (see PR)
- **Done:** three tiers with toasts (bottom-right, no focus), a modal queue, buttons on toasts/modals/centre rows, keyed replace/withdraw, and the collab invitation / join-failure cases from the request.
- **Files touched:** `NotificationStore.{h,cpp}` (model + policy), `NotificationPopups.{h,cpp}` (new, drawing), `NotificationBar.cpp` (rows with buttons), `TeamController.cpp:~550,~690` (invite, join), `CollabController.cpp` `announceFailure` (~2172), `EditorApplication.cpp` `saveSceneToPath`/`openScene`, `EditorUI.cpp` (`doSaveScene`, guarded Save), `SceneAutosave.cpp`/`AssetAutosave.cpp`, `GitController.cpp`, `ContentBrowserPanel.cpp` `notifyFileFailure`.
- **Verified:** store logic + new tests compiled and run by hand with g++/doctest: `tests/test_notification_store.cpp` 22/22 pass; `tests/test_notification_popups_ui.cpp` 10/10 pass headless against real ImGui (toast position and focus, button click reaches the store, Escape, modal waits for another modal, withdraw closes modal) with BMP frames inspected; `python3 scripts/editor_help_audit.py --check` passes; `-fsyntax-only` clean for every changed editor `.cpp` and the changed tests.
- **Not verified:** no full CMake build or `ctest` (configure needs network for FetchContent here). Not run: `test_collab_controller` (two new cases at the end of the file), `test_editor_rewards` (levels renamed), `test_mcp_tools_taskboard`, and the editor itself. The existing collab tests that assert `notes.snapshot().empty()` were reasoned about, not run.
- **Decisions taken:** log-bridge errors are `Important` with `popup=false` (a toast per log line would bury the editor); log `Critical` records do get a modal. Bell ring now fires for Important and Critical. `Info` is never a popup.
- **Decisions suggested, not taken:** see "Not done" 6 and 7.
- **Traps:** a toast's close button must be drawn before the text and the cursor sent back (an ImGui assert fires otherwise — found by the UI test). An action's `run` must not capture a pointer to something a panel can close. Never take the store's lock inside a log sink (see header).
- **Next steps:** 1. Build `he_tests` with a full toolchain and run `test_notification_store`, `test_notification_popups_ui`, `test_collab_controller`, `test_editor_rewards`. 2. Do "Not done" 1 and 3. 3. Add the UI hook for "Open Collaboration" (item 5).
