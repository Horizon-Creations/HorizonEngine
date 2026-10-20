# Collab sync audit, step 9: attach to topic 199 (team server as source control)

Topic 198, step 9. Topic 199 (team server as source-control mediator) exists only as a
design so far: `docs/team-server-source-control-design-2026-10-13.md` on branch
`claude/team-server-als-source-control-mittelsmann-opt-in-auto-merge`, no code. So this step
builds the editor-side attach points to that design's exact shape. When 199's editor step
lands, it fills the upload slot instead of inventing its own.

## 1. What exists now

| Piece | Where | What it does |
|---|---|---|
| Opt-in | `ProjectSettings.h:407` `ProjectSourceControlSettings`, `ProjectSettings::sourceControl` | `Config/ProjectSettings.json` `"sourceControl": {"mode": "git"\|"teamServer", "team", "project"}` (design §6.1). Default `git`, part of `isDefault()`. An unknown mode string reads as `git` |
| Gate | `GitController.h:226` `setTeamServer`, `blockedByTeamServer()`; `GitController.cpp` `mayModify()` | In `teamServer` mode the editor writes nothing to git (commit, pull, push, init, branch, discard...). Status, history and auto-fetch stay read-only |
| Per-frame push | `EditorApplication.cpp:4745` | The project setting goes to GitController before `m_collab.update`, so this frame's settle hand-over sees it |
| Hand-over | `EditorApplication.cpp:736` `onSessionSettled` → `GitController::handOverToTeamServer` | The step 8 hook (host: save anywhere in the session, or 5 s idle) passes the settled keys (asset rel paths, `::Scene::`). With `git` mode: dropped |
| Upload slot | `GitController::setTeamServerUpload(fn(keys) → bool)` | Topic 199's `TeamSyncService` fills it. Until then the keys wait in `teamServerPending()` (deduplicated, oldest first). `update()` re-offers them every frame; `true` = taken, recorded as `teamServerLastHandedMs/Count` |
| Notice | `SourceControlPanel.cpp:1476` `drawTeamServerNotice`, drawn at `:1578` | At the top in `teamServer` mode: team/project, "merges, commits, backs up to GitHub, nothing to commit here", the session line (host: waiting N / handed N s ago / "you host this session"; client: "the host hands everyone's saves over"). Plus a warning while the slot is empty ("cannot reach the team server yet... switch back to git in ProjectSettings.json"). Missing git or no repo: notice only, no install or `git init` offer. The guest notice is not repeated |
| Preferences | `EditorSettingsPanel.cpp:1155` | Source Control page explains instead of offering repo setup (before the missing-git line) |

## 2. Decisions

* **Gate in `mayModify()`**, not just hidden buttons: the design §6.1 says the editor no
  longer commits or pushes to a local `.git`. One place, like `blockedByCollabSession`, so no
  caller can forget it. A notice "the server commits" next to a live Commit button would have
  contradicted itself.
* **Keys, not a "do the upload" bool**: the design's upload sends everything that differs
  anyway. The keys are a trigger plus something to log and to show.
* **No UI to switch the mode** (Project Settings tab): that is 199's editor step (team/project
  picker, creating the project on the server). Today the flag is reached only by editing
  the JSON. The panel says how to go back.
* **Only the session host hands over** (step 8's settle runs only on the host). Outside a
  session, 199's design triggers on every save itself (§6.3). This step does not do that.

## 3. Verified

* `out/build/release`: `he_tests` and `HorizonEditor` build rc 0 (the panel files are only
  in the editor).
* `test_project_settings.cpp` 26/26 (4 new: legacy file = git and fileless, design shape round
  trip, unknown mode = git, real save/load). `test_git_roundtrip.cpp` 41/41 (2 new: gate +
  hand-over queue/flush/reset; commit request in team mode on a real repo leaves HEAD at 1).
* Negative control: without `&& !blockedByTeamServer()` in `mayModify()` both new
  GitController cases fail (the commit lands, rev-list 2).
* `editor_help_audit.py --check` 1168/1168 (no new controls).
* Full `he_tests` minus material_graph (`HOME=$(mktemp -d)`): 5146/5146. The first run was
  started from `out/build/release/tests` and had 5 `HeprojRegistration (Linux)` failures: that
  file finds `scripts/linux_assets` relative to the CWD. Rerun from `out/build/release`: 36/36.

## 4. Not verified

* The panel and Preferences text were not seen on screen (SourceControlPanel.cpp is not in
  he_tests; no headless shot was taken). The hand-over from a real session settle in the
  editor is build-checked only (the controller side is step 8's tested hook).
* A teammate changing `Config/ProjectSettings.json` mid-session: whether the editor reloads
  the settings is not checked. The push reads `currentProject().settings`.

## Handoff

- **Topic / step:** 198, step 9 of 11
- **Branch:** `claude/audit-vollst-ndige-bestandsaufnahme-was-ber-live-collab-sitz`
- **Done:** section 1. Posted to topic 199 (post 1370) so their editor step builds on
  `ProjectSourceControlSettings` / `setTeamServerUpload` instead of adding them again.
- **Files:** `ProjectSettings.h:407`, `ProjectSettings.cpp` (==, toJson, fromJson);
  `GitController.h:206-250`, `GitController.cpp:145`, `:314`, `:448-487`;
  `EditorApplication.cpp:736`, `:4745`; `SourceControlPanel.cpp:1476`, `:1578`, `:1656`;
  `EditorSettingsPanel.cpp:1155`; tests `test_project_settings.cpp:642-718`,
  `test_git_roundtrip.cpp:1747-1846`.
- **For step 10 (audit):** under `teamServer`, everything that writes git is off by the gate.
  Still running and worth a decision there: auto-fetch (read-only, would fetch an `origin` the
  team no longer uses), the History/Branches tabs (read-only view of a stale local history),
  the Content Browser git badges (status of a local `.git` nobody commits to), GitHub sign-in
  in Preferences (now useful only for the server's backup repo, §7). Also: is a client in a
  session that saves expected to upload itself (design §2 says no: clients pause, the host
  saves for all; matches this wiring).
- **Traps:** `GitController::closeProject` resets team mode; the per-frame push turns it on
  again for the next project. `setTeamServer(false)` drops queued keys on purpose.
- **Next steps:** 1. Step 10 audit (list above). 2. Step 11 live: set `"sourceControl":
  {"mode":"teamServer","team":"t","project":"p"}` in a test project, host a session, save as
  a client; the host's panel shows "waiting for the team server: 1 document".
