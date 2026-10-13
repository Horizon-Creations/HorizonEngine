# Git auto-install, step 3: starting the install, visibly, with a fallback

Topic 195. Step 1 found the call sites (`docs/git-auto-install-callsites-2026-10-13.md`),
step 2 built the locator and the per-platform plan (`docs/git-auto-install-detection-2026-10-13.md`).
This step runs the plan.

## 1. What runs, per route

`HE::Sc::gitInstallLaunch(plan)` (`src/HE_SourceControl/include/SourceControl/GitInstall.h`)
turns the probe's `GitInstallPlan` into an argv launch. Pure, tested in `tests/test_git_locator.cpp`.

| Route | Process | The user sees |
|---|---|---|
| Winget | `winget install --id Git.Git -e --source winget --accept-source-agreements --accept-package-agreements` | UAC prompt from the Git installer |
| Homebrew | `brew install git` | nothing beyond the log |
| XcodeCommandLineTools | `xcode-select --install`, then a wait (max 60 min, 1 s polls) for `/Library/Developer/CommandLineTools/usr/bin/git` | Apple's installer window |
| Apt/Dnf/Pacman/Zypper + pkexec | `pkexec /bin/sh -c '<line>'` (apt: `export DEBIAN_FRONTEND=noninteractive; ` prefixed) | polkit password dialog |
| … as root | `/bin/sh -c '<line>'` | nothing |
| DownloadInstaller (Windows, no winget) | **nothing**: download page + "run it, then Recheck" | browser |
| Linux without pkexec/root, or no route | **nothing**: reason + `manualCommand` (with sudo) to copy + download page | |

`HE::Sc::runGitInstall(plan, onLine, cancel)` runs it (45 min timeout), streams stdout and stderr
line by line, then `invalidateGitLocation()` and `git --version` through the locator. The verdict is
that `--version`, not the installer's exit code (winget reports "already installed" as non-zero).
The macOS shim is never run: `developerToolsPresent()` (moved from GitProbe.cpp's anonymous namespace
into `GitProbe.h`) is asked first. pkexec 126/127 (cancelled / no agent) get their own message.

## 2. Editor wiring

* `EditorApplication::startGitInstall()` (`EditorApplication.cpp`, beside `applyGitIdentity`): worker
  with its own log/flags (`m_gitInstall*`), re-probes inline when done. Every probe that finds git
  (this one, or Recheck after a manual install) raises `m_gitReopenRepo`. The frame loop (before
  `m_git.update`) then calls `m_git.requestRefresh()` when `m_git.gitMissing()`. Shutdown raises
  `m_gitInstallCancel` and joins. `startGitProbe()` is a no-op while an install runs (both would
  write `m_gitProbe`).
* "The original function then runs through": `GitService` remembers an `Open` that had no git
  (`m_openPath`, `m_openedWithoutGit`). The next status refresh that finds git runs the skipped
  `findRepoRoot` itself, so an existing repository appears without close/open (a close joins the
  worker and would block behind a running push).
* AppContext: `startGitInstall`, `gitInstallLog`, `gitInstalling/Done/Ok`.
* `GitMissingDialog::drawGitInstallRemedy(AppContext&)`: plan-driven. Install Git + live log
  when `gitInstallLaunch` gives a launch, otherwise the fallback. Opens its own "Source Control" help
  scope. Drawn in: the startup dialog, Preferences ▸ Source Control, the clone dialog, and (new) the
  Source Control panel.
* `GitService` `Open` asks `gitExecutable()` first and reports `Event::gitMissing`; a status refresh
  with no root re-asks it, so the flag does not decay into "no repository". `GitController::gitMissing()`.
  The panel checks it (and `gitProbe->gitFound`) BEFORE `!isRepo()`, so an existing repo is no longer
  offered `git init` while git is missing.

## 3. Decisions

* **Install starts on a click, not by itself.** Every git-dependent place shows the Install Git
  button in place, so the first one the user reaches is where it starts. Fully automatic would
  mean Homebrew installing 100+ MB with no prompt at all because the editor started.
* **Windows without winget = guided download, no silent download.** The engine ships no
  installer and does not fetch and run executables. Fresh Windows 10/11 has winget, so the
  done-criterion is met by the winget route.
* **No Git LFS install** (queen's decision still open). Only Git for Windows brings LFS. On macOS
  and Linux the startup dialog shows the LFS remedy after git is in.

## Handoff

- **Topic / step:** 195, step 3 of 4. Branch `claude/engine-auto-git-install`.
- **Done:** `GitInstall.h/.cpp` (launch + run + verify), `startGitInstall` + AppContext fields,
  plan-driven `drawGitInstallRemedy(ctx)` in 4 places (+ Recheck in the Source Control panel),
  `GitService` gitMissing state + self-healing repo detection, help entries, tests.
- **Verified (Mac M5, Release, `out/build/release`):** `HorizonEditor` and `he_tests` build. `he_tests
  --source-file=*test_git_locator*,*test_git_install_ui*,*test_sourcecontrol_probe*,*test_git_roundtrip*,*test_git_panel_ops*,*test_git_status_parse*`
  → 88/88 cases, 1085 assertions (`ctest -R test_git_install_ui` registered too). New: 4 `gitInstallLaunch`/`runGitInstall` cases in
  `test_git_locator.cpp` (every route's argv, apt `&&` as one `sh -c` arg, fallbacks start nothing) and
  `tests/test_git_install_ui.cpp` (headless: Install Git present iff the route can run, a click reaches
  `startGitInstall`, fallbacks show Copy/Download, disabled + log while installing, Cancel only for
  the Apple-installer wait and it reaches `cancelGitInstall`). Frames
  looked at (`HE_UI_DUMP_DIR`). `editor_help_audit --check` green (1145/1145). Editor headless smoke
  run (`HE_EXIT_AFTER_FRAMES=120`): probe line, clean exit, rc 0.
- **Not verified:** no real install ran anywhere. winget / brew / xcode-select / pkexec were not
  executed, and the GitService self-heal was not driven (this Mac always has `/usr/bin/git`, so
  "no git" cannot be simulated in-process). Windows and Linux: CI only.
- **Open (not this step):** `GitResult::gitMissing` (GitCli.cpp:107) still has no consumer (only
  matters if git is uninstalled under a running editor; all UI gates read the probe).
  `ReportIssueDialog` still needs git for the stored token and shows no remedy.
- **Decisions:** click-to-install, no auto-start; Cancel (`AppContext::cancelGitInstall`) only ends
  the wait for Apple's installer, never a running winget/apt; Windows without winget = download page; no LFS install;
  self-heal in GitService instead of close/open (close joins the worker).
- **Traps:** `runGitInstall` really installs: never call it in a test with a runnable plan.
  `editor_help_audit` assigns scopes by reading the file top to bottom, so a nested `Help::Scope`
  in a panel moves every later label in that file into the wrong scope. A shutdown during a running
  winget/apt waits for it (Proc::run cannot be cancelled; 45 min cap).
- **Next steps (step 4):** 1. Fresh Windows VM without git: Install Git → UAC → `git --version` → the
  Source Control panel shows the existing repo (no restart). 2. Fresh Mac: CLT route (Apple's dialog,
  wait loop) and Homebrew route. 3. Linux (Ubuntu desktop): pkexec dialog; also without pkexec
  (fallback text + sudo command); cancel the password (126 message). 4. Queen: decide LFS.
