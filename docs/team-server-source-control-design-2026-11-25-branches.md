# Team sync: conflicts, restore, a new view, feature branches (2026-11-25)

Follows `docs/team-server-source-control-design-2026-10-13.md` (its handoffs still hold).

## Handoff

- **Commits:** HorizonEngine `release/0.8.0` 0e332aa9e (service: conflicts kept, restore, TextMerge),
  628d1a467 (view + conflict window), this commit (branches, editor side). CollabSession
  `claude/he-net-relay-server` 0021909 (BRANCH_CREATE/LIST/MERGE, message as LOG subject). Pushed.
  **Server not deployed**: until it is, branch requests fail with bad-request (the editor says so)
  and the history keeps showing "Save by ..." instead of the message.
- **Conflicts** (`TeamSyncService::lost`, kept in `Saved/TeamSync/<team>-<project>.conflicts`, open and
  resolved): a notification with Resolve per conflict (`TeamSyncController`, key
  `teamsync.conflict.<rev>.<path>`, withdrawn on resolve); the window (`TeamSyncView::drawConflictWindow`)
  fetches our version quietly (`fetchLost(l, false)` → `lostCopyPath`), diffs it against the project's
  file (`SourceControl/TextMerge`), picks per block (Yours/Theirs/Both), refuses a mix that breaks JSON,
  whole-file pick for graphs/assets/binaries; `TeamSyncController::resolve` writes, uploads as
  "Resolve <path> (r<rev>)", resolves. A conflict only exists where the server let a later save win
  (graphs, binaries, the same field/line); scenes and JSON otherwise merge on the server.
- **Restore** (`TeamSyncService::restoreRevision`): to the head first, then every file of rev N written
  back through pump (keep-local, Play), files the team added since removed, uploaded as "Restore rN".
  Hover a history row → restore icon → confirmation. Not in a live session.
- **View** (`TeamSyncView::draw`): status card, branch bar, "To resolve", last save with outcome icons,
  history timeline (member initial, "Saved N files" / the message, this copy marked, click = files).
- **Branches:** server project `<project>--<name>`, forked server-side; the editor's current branch is
  its own (`Saved/TeamSync/<team>-<project>.branch`, read in `TeamSyncController::update`, so the open
  gate follows it too). Switching (`switchBranch`) needs nothing waiting and no session, then mirrors the
  folder exactly (`Settings::mirrorOnStart`: the team's version, extra files removed, no first-sync
  question). `createBranch` (from the project, not from a branch) switches once made; `mergeIntoMain`
  switches back, then BRANCH_MERGE (server 3-way with the fork's files as bases; outcomes in "Your last
  save", conflicts as above).
- **Verified:** he_tests 354/354 (team, source control, help, planner, git, hub, text merge); with
  `HE_TEST_TEAM_SERVER` against the freshly built server 86/86 three times in a row (branches create,
  save, switch both ways with the folder mirrored, merge with "Merge branch lights" in the history);
  CollabSession ctest 7/7 (`sc_test.py` 11/11 incl. `test_branches`); help audit rc 0; HorizonEditor
  builds; shots looked at: view, conflict window, branches.
- **Not verified:** the real editor (clicks, the notification's Resolve opening the window); a merge
  of a branch twice after more work (it re-applies everything since the fork; harmless where main
  already has it, but a branch change main later reverted comes back); branch deletion (none yet).
- **Next:** 1. Deploy (CollabSession `deploy/deploy.sh`). 2. Real-editor pass. 3. Delete/rename a
  branch; move the fork marker forward after a merge.
