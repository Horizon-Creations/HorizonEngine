# Git auto-install: where the engine runs git, and the one place that should ask

Topic 195, step 1 (survey). Found by grep over `src/` (vendor excluded) for
`o.exe = "git"`, `Proc::run`, `popen`/`CreateProcess`, `"git ...` literals and the
`GitCli`/`GitService`/`GitProbe`/`GitHubTokenStore` APIs. No code changed in this step.

## 1. Every process launch of git (the funnels)

Every runtime git process goes through `HE::Proc::run` with `exe = "git"`. That happens in
exactly five places, four of them inside `HE_SourceControl`:

| # | Where | What |
|---|---|---|
| F1 | `src/HE_SourceControl/src/GitCli.cpp:56` `runImpl` | every `GitCli::*` call (status, init, commit, push, pull, fetch, clone, branch, stash, lfs, `credentialHelper`, `ensureCredentialHelper`...). `launchFailed` today becomes the string "git could not be started — is it installed and on PATH?" (`:92`), with no flag |
| F2 | `GitCli.cpp:915` `approveCredential`, `:947` `fillCredential`, `:1007` `rejectCredential` | own `Proc::Options` because they feed stdin; bypass `runImpl` |
| F3 | `src/HE_SourceControl/src/GitProbe.cpp:51` `runCapture` | `probeGit()` (version, lfs, identity, helper) |
| F4 | `src/HE_Editor/EditorApplication.cpp:2357` `applyGitIdentity` | `git config --global user.*`; a stray direct launch, should move onto `GitCli::run` |

## 2. Engine features that reach those funnels (what breaks without git)

| Feature | Entry | Through |
|---|---|---|
| Open project = repo detection | `EditorApplication.cpp:1904` `m_git.openProject` → `GitService.cpp:384` `findRepoRoot` | F1. **Trap:** without git `findRepoRoot` returns empty, so an existing repo looks like "not a repository yet" and the panel offers `git init` |
| Source Control panel (status, stage, commit, push/pull, auto-fetch, branches, stash, history, restore) | `GitController.cpp` → `GitService` worker | F1 + F2 (push/setup store credentials, `GitService.cpp:626-813`) |
| Branch-name check on the UI thread | `SourceControlPanel.cpp:1657-1658` `GitCli::isValidBranchName/branchExists` | F1, synchronously on the frame thread |
| Create repository | `EditorSettingsPanel.cpp:1213` `requestInit` | F1 |
| Clone from GitHub | `GitCloneDialog.cpp:438/492` → `GitController::requestClone` | F1 + F2 |
| GitHub sign-in (token kept in git's credential helper) | `GitHubSignIn.cpp:79-135` → `GitHubTokenStore` | F1 + F2 |
| GitHub repo list | `GitController.cpp:64` `GitHubTokenStore::load` | F2 |
| Report Issue (reads the stored GitHub token) | `ReportIssueDialog.cpp:459` `GitHubTokenStore::load` | F2. Easy to miss: not a "source control" feature |
| Startup probe + identity | `EditorApplication.cpp:2175` `startGitProbe`, `:2346` `applyGitIdentity` | F3, F4 |

Read-only consumers (no launch): `ContentBrowserPanel.cpp` (tile badges from cached
status), `SceneDiskWatch.cpp`. UI gates that already check `ctx.gitProbe->gitFound`:
`GitMissingDialog.cpp:86`, `GitCloneDialog.cpp:296`, `GitHubSignIn.cpp:438`,
`EditorSettingsPanel.cpp:1171/1442/1699`.

**Does not use git (topic text assumed otherwise):** EngineContent server and its sync
(`HE_ContentSync`, SFTP/libssh2), self-update (`HE_SelfUpdate`, SFTP), `HE_Net`/collaboration,
`HE_Python`, `HcCodegen`/project export (no FetchContent at runtime). Topic 166 (accounts /
team server) has no git code yet. Script API `process.which/run` (`EngineApi.cpp:3262`) can
start git from user scripts. That is user space and deliberately outside the check point.

## 3. The single central check point (decision)

In `HE_SourceControl`, new `SourceControl/GitLocator.h` (namespace `HE::Sc`):

- `std::optional<std::filesystem::path> gitExecutable()`: cached, thread-safe (mutex or
  atomic), resolved once with `Proc::which("git")` plus known install dirs. **All of
  F1-F4 launch this absolute path instead of `"git"`**, so nothing can start git around it.
- `void invalidateGitLocation()`: called after an install and by Recheck.
- `GitResult::gitMissing` (new bool), set by `runImpl`/F2 when `gitExecutable()` is empty
  or `launchFailed`. `GitService` turns it into a distinct state/event, never into "not a
  repository" (`GitService.cpp:384`).
- `probeGit()` uses the locator for `gitPath`/`gitFound` and keeps "binary found" separate
  from "`--version` runs" (see macOS below).
- The library **only reports**; it never installs. One editor-side trigger (step 3),
  `EditorApplication::startGitInstall()` modelled on `startToolchainInstall`
  (`EditorApplication.cpp:2384`, `HcCodegen::installToolchain`, `ToolchainDialog`), is
  called from every UI gate above and from a `gitMissing` result.

Constraints the design must carry:
- **Windows PATH is stale after install.** `Proc::augmentToolPath` is a no-op on Windows
  (`Process.h:109`), and the help text says "restart the editor" (`EditorHelp.cpp:8835`).
  For "the original function then runs through" the locator must re-read the registry PATH
  (HKLM+HKCU `Environment`) or probe `%ProgramFiles%\Git\cmd\git.exe` /
  `%LOCALAPPDATA%\Programs\Git\cmd\git.exe`, then launch the absolute path.
- **macOS shim:** a fresh Mac has `/usr/bin/git` as an xcrun stub. `which` finds it,
  `--version` fails, and running it may pop the CLT installer on its own. Not verified here.
- Prior choice being superseded: `GitMissingDialog::drawGitInstallRemedy` (`GitMissingDialog.cpp:49`)
  deliberately only copies a command "to run in your own shell" (`EditorHelp.cpp:8823-8837`).

## Handoff

- **Topic / step:** 195, step 1 of 4
- **Branch / commit:** claude/engine-auto-git-install (this doc only; pushed)
- **Done:** call-site list (sections 1-2) and the central check point (section 3).
- **Verified:** by grep/reading only. `rg 'o.exe\s*=\s*"git"'` gives exactly F1-F4.
  `GitCloneDialog`, `GitHubSignIn` and `EditorSettingsPanel` gate on `gitProbe->gitFound`.
- **Not verified:** no build or test run (no code changed). macOS stub behaviour, and which
  installs bring git-lfs, were not checked on a real machine.
- **Decisions taken:** check point lives in `HE_SourceControl` as a path resolver (not a bool),
  because only an absolute path survives the stale Windows PATH. The library never installs.
- **Decisions suggested, not taken (queen/human):** is **Git LFS in scope**? `GitProbe::ready()`
  requires LFS (`GitProbe.h:55`). Git for Windows bundles git-lfs. Xcode CLT and distro `git`
  packages do not. A git-only install would keep the "Source control unavailable" dialog
  coming back on macOS/Linux.
- **Traps:** `findRepoRoot` without git = "not a repo" + offered `git init` (`GitService.cpp:384`).
  `SourceControlPanel.cpp:1657` runs git on the frame thread. `ReportIssueDialog` silently needs git.
  Existing tests: `tests/test_sourcecontrol_probe.cpp` (version parsing, `probeGit`),
  `test_git_roundtrip.cpp`, `test_git_panel_ops.cpp`.
- **Next steps:** 1. Step 2: add `GitLocator` (+ Windows registry/known-dir lookup) and
   route F1-F4 through it. Add `GitResult::gitMissing` and the `GitService` missing-git state.
   Add doctest cases for the locator with a fake PATH. 2. Extend `GitProbe` with a package-manager
   field (winget / brew / xcode-select / apt|dnf|pacman|zypper via pkexec), reusing
   `HcCodegen`'s detection. 3. Step 3: `startGitInstall()` + dialog, hooked into all UI gates.
