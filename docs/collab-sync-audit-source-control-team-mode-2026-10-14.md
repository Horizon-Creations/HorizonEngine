# Collab sync audit, step 10: source control while the team server keeps the history

Topic 198, step 10. Question: with `sourceControl.mode = "teamServer"` (topic 199, design
`docs/team-server-source-control-design-2026-10-13.md` on branch
`claude/team-server-als-source-control-mittelsmann-opt-in-auto-merge`), what is left of the
editor's source control that still makes sense for one person, what is obsolete, and what
is missing? The result is a **spec for topic 199** (section 3). This step changes no code;
the attach points it builds on are step 9's (`docs/collab-sync-audit-team-server-2026-10-14.md`).

## 1. Every source-control touchpoint, today and in team mode

"Today in team mode" is the state on this branch after step 9 (gate in
`GitController::mayModify()`, `GitController.cpp:448`). "Owner" says who has to act.

| Touchpoint | Where | Today in team mode | Should be | Owner |
|---|---|---|---|---|
| Commit, stage, unstage, amend | `SourceControlPanel.cpp` `drawChangesTab` :870 | Read-only (gate) | **Obsolete.** Every save is the commit (design §3, one revision per upload) | done (step 9) |
| Changes tab list | :870 | Lists `git status` against the frozen local `.git`: after the switch every saved file shows as modified, forever | **Replace**: files that differ from the per-file base (design §6.3 state file), i.e. "waiting to upload" | 199 step 4 |
| Discard | `requestDiscard` | Off (gate) | **Keep, new meaning**: "Revert to the team's version" = GET head's blob for that file, confirmed first | 199 step 4 |
| Conflict resolve (mine/theirs) | `requestResolveConflict` :203 | Off (gate) | **Obsolete**: the server never leaves a conflict. Replaced by the loser list (3.5) | 199 step 4 |
| Pull / Push buttons, ahead/behind | header bar :1296-1400 | Hidden (`hasRemote && mayWrite`) | **Replace** with one sync state: in sync at r N / N waiting / uploading / offline | 199 step 4 |
| Fetch button | header bar :1383, `GitController::requestFetch` :388 | **Still live** (not gated): fetches an `origin` nobody pushes to any more | **Hide** in team mode | 199 step 4 |
| Auto-fetch timer | `GitController::update` :354 | **Still runs** (no team check); an expired token for the unused remote still raises a notification | **Off** in team mode (`&& !m_teamServer`) | 199 step 4 |
| History tab | `drawHistoryTab` :955 | Read-only view of the frozen local history | **Replace** with the server's LOG (op 7) incl. trailers; old local history only as a folded "before the team server" | 199 step 4 |
| Restore to commit | `requestRestoreTo`, :1839 | Off (gate) | **Keep, new source**: restore a file or the project to server rev N (3.4) | 199 step 4 |
| Branches tab, create/switch branch, stash | `drawBranchesTab` :1090 | Read-only | **Obsolete**: the server has one linear `main` (design §3). Hide the tab (3.6) | 199 step 4 |
| Content Browser badges | `ContentBrowserPanel.cpp` :814, :1666, :2023 (`isRepo`, `folderHasChanges`, `entryForAbsolutePath`) | Status against the frozen `.git`: every edited file "M" forever | **Same three calls, new source**: answered from the team-sync state (3.7) | 199 step 4 |
| COMMITTED reward | `EditorRewards.h` `SyncWatch` :669, `GitController::takeSyncMoment` | Never fires (no commit happens) | Fire when **our** upload became a revision (`applied`/`merged`) | 199 step 4 |
| Preferences, Repository section | `EditorSettingsPanel.cpp` :1155 | Early return with an explanation; auto-fetch/auto-push switches are hidden with it | Fine as is; add the team-sync settings (server, pause, see 3.6) there | 199 step 4 |
| Preferences, GitHub account | :1561 `GitHubSignIn::drawAccountRow` | Shown | **Keep**: admins need it to register the backup deploy key (design §7); Report Issue uses it too | none |
| Credential store, git identity, `git init`, set remote, GitHub repo setup | :1206, :1251, :1412 | Off (gate / early return) | **Obsolete** for this project: the author is the team member (design §3) | done |
| Tool setup rows git / Git LFS / identity | `ToolSetupDialog.h` | Still nag at startup when missing | Say "not needed for this project" while a team-mode project is open; the sync needs no git on the client (sha256 + own protocol) | 199 step 4 (minor) |
| Clone from GitHub | `GitCloneDialog.cpp`, Preferences without project | Clones the backup repo like any repo | **Add** "Open team project" (MANIFEST + GET, design §6.1). Cloning the backup must also work (3.2) | 199 step 4 |
| Scene changed on disk | `SceneDiskWatch` (`saveSceneToPath`, `EditorApplication.cpp:12968`) | n/a | Keep: a background pull goes through it (design §6.3) | 199 step 4 |
| Session hand-over | `EditorApplication.cpp:736` → `GitController::handOverToTeamServer` | Keys queue, nothing uploads (no slot filler) | Fill `setTeamServerUpload`; add contributors (3.3) | 199 step 4 + this topic |
| MCP tools | `Mcp*.cpp` | none touch git | nothing | none |

## 2. What is left for one person to do

Manual commits go away, and so do branches, stash, pull and push. What still needs a
human, and nothing else, is:

1. **See what has not reached the team yet**, and press "Sync now" (design §6.3 has the button).
2. **Write a note for the next sync** (optional). COMMIT already carries `message str16`;
   default stays "Save by <member>". This is all that is left of the commit message.
3. **Throw a local change away**: revert one file to the team's version.
4. **Go back in time**: browse the server's revisions, restore a file or the project to one.
5. **Get a lost version back**: when a later save won, open or re-apply mine from
   `horizon-conflicts` (design §5.6, §6.3 toast).
6. **Admins**: backup target and state (ops 9/10), and creating the project (op 8).

## 3. Spec for topic 199

Each item has its check. Items 3.1-3.3 are the ones that lose data if skipped.

**3.1 After a session, a client takes the team's state as its base before it uploads.**
A session client writes the session's state to its own disk: its own saves
(`saveSceneToPath` still writes locally and only *notifies* the host,
`CollabController::noteLocalSave` :4444) and every peer file (`applyAssetBytes`). Its per-file
bases are still from before the session. Its first upload after the session would send the
session state as its own change on an old base, and "later arrival wins" (design §5.1) lets
that stale copy overwrite what someone outside the session committed in the meantime.
Rule: while in a session a client neither uploads nor pulls (design §2 already pauses
uploads; pulls must pause too, see 3.8). On leaving, it fetches HEAD + MANIFEST first. A file
the session touched whose bytes equal head gets head as its base without an upload. One that
differs gets head's version (the host has kept the session's state, step 8) unless the client
changed it after leaving. *Check:* A and B in a session edit `Main.hescene`. The host uploads.
C (not in the session) changes another field. B leaves and saves something else. C's field
survives in head.

**3.2 First sync without a state file.** This covers a project just switched to team mode,
a clone of the backup repo, and a member's second machine. Without a state file every file
looks like an add. That means an add/add merge for the whole tree, `replaced` outcomes, and
a whole tree of losers on `horizon-conflicts` for files that are actually identical.
Rule: with no state file, MANIFEST head first. A file whose sha256 equals head's is based on
head. Only real differences upload, as `M` against head (the user is asked first if more
than a handful differ). *Check:* clone the backup, open it. The first sync uploads nothing
and `horizon-conflicts` stays empty.

**3.3 A session's revision names everyone who worked in it.** Today the host's upload would
record every participant's edits under the host's name. `CollabSettle::Settled`
(`CollabSettle.h:44`) carries only key and reason. Rule: the host passes the participants who
changed each key with the hand-over (`handOverToTeamServer(keys, contributors)`; the
controller knows who sent deltas, saves and patches). The server writes git-native
`Co-authored-by: <member> <member@<team>.horizon-team>` trailers, which GitHub renders, plus
`Horizon-Session: <names>`. The attribution part belongs to this topic (step 11 or a
follow-up). The trailers belong to 199's server step.

**3.4 Revert and restore without a new protocol op.** Ops 1-10 have no restore. Compose it
on the client: MANIFEST(rev N) → GET the blobs → upload them as a normal COMMIT with the
message "Restore <path|project> to r N". The restore is then itself a revision you can undo,
as `requestRestoreTo` is today. "Revert my change" is the same for one file at head.
*Check:* restore a file to r N-2 and the next LOG entry says so. Restoring r N-1 again
undoes it.

**3.5 The loser list replaces conflict resolution.** In the team-sync view, the revisions
where one of *my* files lost something (outcomes `merged-arrival`, `replaced`,
`delete-dropped`), each with "Open my version" (GET the preserved blob read-only) and "Use
my version" (upload it as a new revision, 3.4).

**3.6 No branches. Say so in the UI.** Experiments go in a second project or a switch back
to git mode (3.9). A "pause sync" switch keeps saves local. It must warn that a long pause
means the paused fields beat newer ones at the next sync ("later arrival wins" judges by
arrival, not by edit time). Spec it with a visible "paused, N waiting" state, or leave it out.

**3.7 Badges come from the team-sync state behind the same three calls.** In team mode,
`GitController::isRepo` / `folderHasChanges` / `entryForAbsolutePath` answer from the
TeamSyncService state: worktree `Added` / `Modified` / `Deleted` against the per-file base,
`Untracked` for never uploaded files. The Content Browser stays unchanged. Until then the
badges should be off in team mode, because "M on everything" teaches users to ignore them.

**3.8 A pulled file on the session host goes into the session.** A background pull (`sc-head`)
writes files under a running session. On a client that would fork it from the host, so
clients do not pull in a session (3.1). On the host, a pulled file is "a file changed under a
loaded asset" (lesson on `applyAssetBytes`: unload + load, `m_reloadedByPeer`). It must also
be sent to the peers through the whole-file asset channel and marked with
`noteSessionTouched`, or late joiners miss it (step 7's catch-up only replays touched files).
The open scene goes through `SceneDiskWatch` on the host. Its reload then reaches peers like
any world replacement. *Check:* a session host gets a pulled material, and both peers show it.

**3.9 Switching, both ways.**
* git → team: the flag sits in a synced file (design §6.1), so the switch is the **last
  direct git commit**. The admin creates the server project and uploads r1 (op 8), commits and
  pushes `ProjectSettings.json`. Everyone pulls once and is in team mode with 3.2's bootstrap.
  The UI should offer this as one action. A member who keeps pushing to the old origin is on
  a dead branch. Back up to a fresh branch (design §7, `horizon-backup`) so the server never
  collides with them.
* team → git: the local `.git` froze at the switch, and the backup repo's `main` has the real
  history. The way back is the backup repo: set it as the remote, fetch, mixed-reset to its
  branch (working tree untouched, so it shows clean). Never "flip the JSON and carry on"
  with the frozen `.git`: its Commit button would push the whole difference as one commit
  into the old origin. Step 9's panel text currently says to flip the JSON. Reword it once
  199 has the action.

**3.10 Offline.** Saves keep landing on disk. The state file keeps the bases, and the next
connection sends one COMMIT with everything that differs. The view shows "offline, N files
waiting" (the same as step 9's "waiting for the team server: N"). Nothing is lost, but a long
time offline means a big merge with arrival winning. Say so after a day.

**3.11 Small gates for 199's editor step**, with no behaviour of their own until then:
auto-fetch off (`GitController.cpp:354`, add `!m_teamServer`), the Fetch button hidden
(`SourceControlPanel.cpp` ~1383), Branches tab hidden (:1711), History tab switched to
LOG (:1706), the reward's source and the tool setup rows (both in the section 1 table).

## 4. Not verified

* Nothing was run: this is a code-reading audit (greps and the line ranges cited above).
* That a client writes peer files to disk was taken from steps 5/7 (`applyAssetBytes` writes,
  then loads). It was not re-tested here. That its own scene save writes locally was read at
  `EditorApplication.cpp:12968-13001`.
* Whether `Co-authored-by` renders for e-mail addresses GitHub does not know: GitHub shows the
  name, no avatar. Not tried.

## Handoff

- **Topic / step:** 198, step 10 of 11 (audit, docs only). Branch
  `claude/audit-vollst-ndige-bestandsaufnahme-was-ber-live-collab-sitz`.
- **Done:** section 1 (every touchpoint with file:line, today vs. should be, owner), section
  2 (what is left for one person: 6 acts), section 3 (11 spec items for 199). Posted to topic
  199 as a decision.
- **Verified:** by reading only (the files and lines in section 1). No build, no test. No code
  changed, so CI is not needed for this step.
- **Decisions taken:** no code in this step. The gates in 3.11 are listed for 199's editor
  step, not built here (without TeamSyncService they would hide things with no replacement,
  and there was no live run to check them). The local `.git` counts as **dormant** in team
  mode, which decides the History, Branches, badge and Fetch rows at once.
- **Only suggested (199 decides):** composing restore on the client (3.4) instead of a new op;
  `Co-authored-by` trailers (3.3); leaving out "pause sync" (3.6).
- **Traps:** 3.1 is a real lost-update path (a stale client copy beats a newer head field)
  that the design does not cover yet. Step 9's panel text "switch back to git in
  ProjectSettings.json" leads to the frozen `.git` (3.9).
- **Next steps:** 1. Step 11 (verification) can add the contributor list to
  `onSessionSettled` → `handOverToTeamServer` (3.3, editor side) if it fits. Otherwise make it
  a follow-up step in this topic. 2. 199 step 4 takes 3.1, 3.2, 3.4-3.11. 199's server step
  takes the trailers from 3.3.
