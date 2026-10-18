# Tool setup dialog: one list for everything the editor needs from outside

Branch `claude/missing-tools-dialog-design-u43dnt`, based on `release/0.7.0` @ `98871840`
(the git auto-install, topic 195). Design review of the two startup dialogs, then a
single replacement.

## 1. What was wrong

Two startup modals said the same thing in two shapes: `ToolchainDialog` (cmake, compiler;
Install Automatically with a raw log, loose copy-command buttons) and `GitMissingDialog`
(git, Git LFS, identity; a separate shape and a separate log). Three more places drew the
git remedy in a third shape, and four places only described a missing tool in grey text
(Export "Compile HorizonCode", Project Hub C++, the clone dialog, Tool Status).

## 2. What it is now

`src/HE_Editor/ToolSetupDialog.{h,cpp}`: **one modal, one row per tool** (cmake, C++ compiler,
git, Git LFS, git identity).

* Row = state icon (drawn, no glyph font needed), what it is for, the package manager and
  package that will run, what the user will see (UAC, polkit dialog, Apple's installer),
  and the one action that fits the state: Install, Retry + Show Log, Cancel (Apple's
  installer wait only), or the identity form.
* Progress: package managers report no percentage, so a running row shows the phase
  (Preparing > package manager > Verifying), an indeterminate bar and the running time.
  The raw output is the "Install log" fold. A failure shows the installer's last line.
* **Install All Missing** works the rows off one installer at a time (the two workers have
  separate logs): cmake + compiler in ONE toolchain call (one password prompt), then git,
  then Git LFS unless git brought it. A failed step empties the queue.
* Commands for doing it by hand are folded into "Manual install".
* Opens by itself once when a probe finds something missing and that part was not silenced
  (`SuppressToolchainWarning` / `SuppressSourceControlWarning`, both set by "Don't show this
  at startup"), and on `requestShow(tool)`. Closes by itself when it was open with something
  missing and everything is found.
* `drawMissingBanner(ctx, tool, text)`: the inline notice with "Set Up…" that opens the
  dialog on that row. Used in Export (Compile HorizonCode) and the Project Hub (C++).
  Preferences ▸ Tool Status "Fix" opens the dialog on the row instead of a page.
* **Git LFS auto-install** (`HE::Sc::GitInstallTarget::GitLfs`): same routes as git, package
  from `GitInstallPlan::lfsInstallCommand` (winget `GitHub.GitLFS`, `brew install git-lfs`,
  the distro's `git-lfs`). No route for the downloadable Windows installer (LFS is inside it)
  or the Command Line Tools (Apple ships none): those get the manual command.
  `AppContext::startGitLfsInstall` / `gitInstallIsLfs`.
* `GitMissingDialog` keeps only the in-place remedy (`drawGitInstallRemedy`,
  `drawLfsInstallRemedy`, now both take `ctx` and both have an Install button). `ToolchainDialog`
  is deleted.

## Handoff

- **Topic / step:** tool setup dialog (follow-up to 195), step 1 of 1 (design + implementation)
- **Branch / commit:** `claude/missing-tools-dialog-design-u43dnt` (pushed: see the PR)
- **Done:** the single dialog, banners, Git LFS install, help entries, tests (section 2).
- **Files touched:** `ToolSetupDialog.cpp` (rows, queue, banner), `GitMissingDialog.cpp` (remedy
  for git and LFS), `GitInstall.cpp:launch/run` (target param), `GitLocator.cpp:planGitInstall`
  (`lfs*Command`), `EditorApplication.cpp:startGitInstall(bool lfs)`, `EditorUI.cpp` (one
  call), `EditorSettingsPanel.cpp` (Tool Status "Fix"), `ExportDialogPanel.cpp`,
  `ProjectHubPanel.cpp` (banners), `EditorHelp.cpp` ("Tool Setup/*"), `scripts/editor_help_audit.py`
  (scans `ToolSetupDialog.cpp` now).
- **Verified:** Linux/GCC 13 container. Locator tests (16 cases incl. the new LFS launch case),
  `test_tool_setup_ui` (14 cases, every row state, per-row callbacks, queue order, failed step,
  banner opens the dialog, auto-open once), `test_git_install_ui` (5 cases incl. LFS remedy),
  help tests that pin labels, `editor_help_audit.py --check` exit 0 (settings area now 174/174).
  All changed editor TUs compile. Frames: `HE_UI_DUMP_DIR=<dir>` writes BMPs.
- **Not verified:** no `he_tests` / `HorizonEditor` full link (only per-file object builds and
  small test binaries linked from he_tests objects). No real install on any OS, so the LFS
  routes are covered by argv tests only. Nothing on Windows or macOS. The "topics exist" help
  test needs the docs objects and was not linked here.
- **Decisions taken:** all installs through the package manager (winget, Homebrew / Command
  Line Tools, apt/dnf/pacman/zypper behind pkexec), no downloaded executables. One installer at
  a time, queued in the dialog. Progress as phases, not percentages. The in-place git remedy
  stays in Preferences, the clone dialog and the Source Control panel (own tests).
- **Decisions suggested, not taken:** replace those three in-place remedies with the banner;
  a Homebrew row on macOS (today cmake's install bootstraps Homebrew in Terminal when it is
  missing, and the row only says so in its prompt line); credential helper as a sixth row.
- **Pending:** the McpClientSetup text blockers (Claude CLI, Python) are not rows.
- **Traps:** `linux` is a predefined macro on GCC (do not name a test helper `linux`). The
  dialog keeps state in file statics; tests call `resetStateForTests()`. AppContext is built with
  designated initialisers, so a new field goes in the same order in `EditorApplication.h` and
  `makeContext`. `ImGui::IsPopupOpen` needs a current window, so ask it inside a frame.
- **Next steps:** 1. Windows (winget + UAC) and macOS (Homebrew, Apple installer) run of the
  dialog with a real missing tool. 2. Decide on the in-place remedies and a Homebrew row.
  3. Link `he_tests` once in CI and run `test_tool_setup_ui`, `test_git_install_ui`, `test_git_locator`.
