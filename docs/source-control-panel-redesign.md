# Source Control panel redesign (2026-10-09)

The panel used to be a header bar, one commit box that committed *everything*, a
read-only change list and a fixed-height history. It is now three tabs under the
same header bar (branch pill, fetch/pull/push with counters, view toggle, options).

## Changes tab
* **Commit box** (multi-line) with a split button. Files ticked = those files are
  committed (`GitService::requestCommitStaged`); nothing ticked = everything, as
  before (`requestCommitAll`). The arrow offers *Commit & Push*, *Commit everything*
  and *Amend last commit* (only while that commit is unpushed).
* **Rows**: checkbox (stage/unstage), type icon, name, folder, git state letter.
  Hover: *Show in Content Browser* and *Discard* (*Delete* for new files, both
  behind a confirm dialog). Double-click = show in Content Browser. Conflicts get
  *mine* / *theirs* buttons instead.
* **Groups**: Conflicts, Staged, Changes, New files - collapsible, with a count and
  *Stage all / Unstage all / Discard all / Delete all* (acting on what the filter shows).
* **Filter**: free text plus chips per asset type (Scenes, Materials, Textures,
  Meshes, Code, Audio). The type comes from the extension, or from the HAsset header
  for `.hasset` (`EditorAssetTypeCache`).

## History tab
Graph strand (green dot = not pushed), click a commit to list its files
(`GitCli::commitFiles`), right-click for *Create branch…* / *Restore…* (unchanged).

## Branches tab
Local branches with *Switch*, branches that exist only on the server with
*Check out* (creates a tracking branch), *New branch…*, and stashed changes with
*Bring back stashed changes*. Switching with uncommitted work asks: **Stash & switch**
or **Carry them over**.

## Backend
`GitCli`: `stage / unstage / discard / resolveConflict / switchBranch /
listRemoteBranches / stashPush / stashPop / stashList / commitFiles / hasHead`, and
`commit(..., amend)`. All paths go to git as *literal* pathspecs and long lists are
split to stay under the Windows command-line limit. `GitService` runs them on the
worker; `GitController` gates them with `mayModify()` (a collaboration guest sees the
tabs read-only).

Staging runs the same LFS size pass as commit-all (`routeLargeFiles`) *before*
`git add`: a file added to the index before it is routed into LFS would be stored as
a plain blob. `CommitStaged` repeats the pass for files that reached the index some
other way and re-adds those with `--renormalize`.

Tests: `tests/test_git_panel_ops.cpp` (real repositories, no mocks).
