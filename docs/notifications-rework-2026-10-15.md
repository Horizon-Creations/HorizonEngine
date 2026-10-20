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

## Follow-up (same day): Build window, sync errors, popup switch

- **Build window can be hidden** (`BuildProgressDialog.cpp`): "Hide" (while running) puts the window away and the run continues. A footer cluster (label, bar, percent; `FooterWidth`/`DrawFooter`, left-most of the right group in `EditorUI.cpp`) shows progress; a click brings the window back (`requestOpen`). A run that ends while the window was not up raises an Important "Export finished/failed" with Show Build (+ Start Game when runnable) via `announceFinish`; a run that ends in front of the user raises nothing. Game-logic builds keep the panel's own notification (the failure one now has "Show Build").
  **Export lock** (`Platform/ProjectWriteGate.h`, header-only in HE_Core): the Build modal used to be the lock on `Content/` while an export packs; hidden, it is a rule instead. While `BuildProgressDialog::exportRunning()` (an EXPORT, not a game-logic build) is true, writers ask `HE::ProjectWriteGate::refuse("save this asset")` and stop: `ContentManager::saveAsset` (the choke point of the ~55 asset saves; the edit stays dirty in memory), `Importer::importSource` / `reimport`, `EditorApplication::saveSceneToPath`, the Content Browser's create/delete/rename/move/revert/import, and Git's discard / resolve-conflict / switch-branch / stash-pop / restore / pull. The editor's handler (`onProjectWriteRefused`) posts one "Export in progress" toast (key `export.lock`, rate-limited to once per 10 s, Show Build button). Not covered: files peers write into the host's project during a collaboration session, Git operations from outside the editor, and anything that writes without going through the places above (a panel that serialises its own file).
- **EngineContent sync failures** (`EngineContentSync::setFailureListener`, `SyncFailure`): the library reports manifest / cache / download failures; the editor listener (`onContentSyncFailure` in `EditorApplication.cpp`) posts ONE aggregated, keyed notification for passive downloads ("N EngineContent files could not be downloaded") and one for a catalogue that cannot be cached. Manifest and explicit-download failures are skipped there because their callers already say what they mean. The three catalogue notifications (server unreachable / unreadable / refresh failed) got a **Try again** button (`refreshEngineContentManifest`) and resolve when it works. The per-file passive notify in `EditorApplication.cpp` was removed (the listener covers it).
- **Popup switch**: Preferences ▸ Feedback ▸ "Show Notification Popups" (`EditorConfig::NotificationPopups`, saved as `NotificationPopups`, catalog row `notifications.popups`, help entry, reset-to-defaults). Pushed every frame into `NotificationStore::setPopupsEnabled`: off = every notification is centre-only whatever its tier, popups that are up come down (rows stay unread), buttons stay usable on the centre rows. Turning it on again does not replay what arrived meanwhile.

## Follow-up 2: inline outcome texts removed

Outcome messages that now have a notification are no longer also printed in the panel:
Collaboration panel (session failure / removed-by-host block with Rejoin+Dismiss, avatar error, "join again" hint), Team window (the "Recent" list, the sign-in error line, the picture error), Source Control panel (last-operation info line), and the "Not saved: …" lines of the Audio, Blend Space, Bone Mask, Cinematic, Sequencer, Theme and Type asset panels. Their sources now post: `TeamController` refusals (validation, "sign in first", "session already running", …) via `notice()`, picture refusals, the asset-save failures (Important, "Not saved"). `TeamController::lastError()` / `pollNotice()` stay (tests and the footer tooltip use them).
Deliberately kept inline: validation next to a field (folder name, branch name, address parse), persistent state ("Runs interpreted.", "Detached HEAD", Team Sync "Last problem", Collab op rows), and messages inside a dialog that is itself the answer (Build window result line, Asset/Scene recovery, guarded-Save "The scene could not be saved.", Self-update, Publish).

## Follow-up 3: more inline outcome texts

Converted to notifications (said once, the slot is emptied): the heightmap result (Terrain), clone result and repository-list error (Git Clone dialog; the dialog keeps a one-line state and the LFS button), GitHub sign-in failure reason, the unsaved-changes guard's "Not saved", asset-recovery restore failure, project-open failure (hub; the dead `##EditorOpenError` modal in `EditorUI.cpp` was removed), project-create failure (hub, editor and tutorial; the two field checks stay inline), Audio Extract result, Streaming split/merge message, Profiler dump load error, Claude registration result (the "What was run" transcript stays), and the Git info line on the Preferences page.
Kept on purpose (state, validation, or the dialog's own answer): Task Board "could not be saved / unreadable" banner (the board has unsaved edits and Reload / Retry Save act on that state), Build window result line, Export "Last export" summary, Publish dialog result, Self-update, Team Sync "Last problem", Texture Viewer load state, compile banners, "Runs interpreted", "Detached HEAD", rename-field validation, `HcExtractUi` field errors, Report Issue phase text.

## Not done (candidates, from the gap survey; all verified by reading in the survey, none changed)

1. ~~Export build result only in the modal~~ done in the follow-up above.
2. ~~EngineContent sync failures log-only~~ done in the follow-up above; a "Retry" for passive downloads is still missing (the registration callback has already answered `false`).
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
