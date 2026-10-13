# Git auto-install — step 4: verification (2026-10-13)

Topic 195, step 4 of 4. Branch `claude/engine-auto-git-install`. Run on NN-WS03 (Windows 11,
not elevated, Git for Windows 2.53 installed) and in WSL 2.6 distros on that machine. Earlier
steps: `git-auto-install-callsites-2026-10-13.md`, `…-detection-…`, `…-trigger-…`.

**Short version:** Linux is verified end to end on a machine that has never had git: every
route, including the no-rights fallback, a real install through pkexec and as root, and
the original function (repository detection + status) running afterwards without a restart.
Windows is verified only in parts. This workstation has git, no admin rights and no
Windows Sandbox, so a real winget install has not run. macOS was not tested at all.

## 1. Build and tests

| What | Result |
|---|---|
| Windows Release (`C:\hw195`, VS18, `-DDEPLOY_DIR=C:/hw195/deploy`): `HorizonEditor`, `he_tests` | build ok |
| `he_tests` full run, scratch APPDATA, `HE_NET_LOOPBACK_ONLY=1` | 5173/5174. **One failure, caused by step 3** (below) |
| after the fix: `--source-file=*test_editor_help*,*test_git_*,*test_sourcecontrol*` | 106/106, 89307 assertions |
| `scripts/editor_help_audit.py --check` | 1145/1145 |
| Linux Release (WSL Ubuntu 26.04, GCC 15.2, `~/hw195`): `HorizonSourceControl`, `he_tests` | build ok |
| Linux `he_tests` git + help files (on 0e086393, before the fix) | 106/107: the same help failure, nothing else |

**Regression from step 3, fixed here:** step 3 renamed the manual-fallback button from
"git-scm.com" to "Download Page" and removed the help entry, but
`tests/test_editor_help.cpp:443` still asked for "Source Control/git-scm.com". The test list
now asks for "Download Page", whose entry is `EditorHelp.cpp:8845`. CI on 0e086393 would have
gone red on this on all three OSes.

## 2. Harness

`docs/git-auto-install-e2e/git_install_e2e.cpp` replays the editor's sequence with the same
HorizonSourceControl calls: `probeGit()`, then `GitService::open(project)`, then
`runGitInstall(probe.install)` (= `EditorApplication::startGitInstall`), then `probeGit()`, then
`requestStatus()` while `gitMissing()` (= `m_gitReopenRepo` in the frame loop). It is not in
CMake and not in he_tests, because it really installs. `linux-fresh-distro.sh` bundles it and
runs it (`bundle` / `run <case>`); its header lists the steps for setting up the distro.

## 3. Linux: fresh `ubuntu-base-26.04.1` WSL distros (no git, no sudo, no pkexec, no Windows PATH)

| Case | Plan | What happened | Logs |
|---|---|---|---|
| user `tester`, no pkexec (**no rights**) | `apt` / `Unavailable` | nothing started. Message: "cannot install git … without pkexec … run the command in a terminal", manual `sudo apt-get update && sudo apt-get install -y git`. GitService: `gitMissing=1`, not "no repo" | `C:\hw195\e2e-linux\case-norights.log` |
| `tester`, pkexec installed, no polkitd/agent (headless, SSH, WSL) | `apt` / `Pkexec` | pkexec exits **127** right away ("Error getting authority"). Message: "pkexec could not ask for a password (no authentication agent is running). Run the command below in a terminal instead." No hang | `case-pkexec-nobus.log` |
| `tester`, pkexec + system bus + polkitd, rule allows `tester` (stands in for the password dialog) | `apt` / `Pkexec` | `pkexec /bin/sh -c '…apt-get install -y git'` **installed git (8.1 s)**, `git --version` = 2.53.0, GitService logged "git is available now; repository: …", `isRepo=1 branch=main` without a restart | `case-pkexec-polkit.log` |
| root, second fresh distro | `apt` / `AlreadyRoot` | `/bin/sh -c` apt **installed git (12.0 s)**, 2.53.0, repo `main` detected again without a restart | `case-root.log` |

After every successful install the probe reports `lfs MISSING, identity MISSING`. The
"Source control unavailable" dialog therefore stays open, now showing the LFS remedy and
the identity form, although git itself works. That follows from `GitProbe::ready()`, which
requires LFS, and the LFS decision is still with the queen.

## 4. Windows (NN-WS03): parts only

Same harness, MSVC, **without** `--install` (inside a VS18 vcvars64 shell, then copy
`C:\hw195\tests\*.dll` next to the exe):
`cl /std:c++20 /EHsc /O1 /MD /I src\HE_Core\include /I src\HE_SourceControl\include docs\git-auto-install-e2e\git_install_e2e.cpp /link /LIBPATH:C:\hw195\src\HE_SourceControl /LIBPATH:C:\hw195\src\HE_Core HorizonSourceControl.lib HorizonCore.lib`

* Normal PATH: git `C:\Program Files\Git\cmd\git.exe`. Route `winget`/`OsPrompt`, launch
  `winget install --id Git.Git -e --source winget --accept-source-agreements --accept-package-agreements`.
* PATH stripped of every `Git` and `WindowsApps` entry (`Get-Command git` finds nothing): the
  locator still finds `C:\Program Files\Git\cmd\git.exe` through the registry PATH, and
  GitService detects the repository. This is the "installed while the editor runs" path. Route:
  `Git for Windows installer`, no launch, download page `https://git-scm.com/download/win`
  (the Windows fallback).
* Side note: `GitInstallPlan::canInstallAutomatically()` is `true` for `DownloadInstaller`
  (the test `test_git_locator.cpp:182` pins that), although the engine never runs it. Only
  `gitInstallLaunch` reads it, and the UI keys on the launch, so there is no visible effect,
  but the name is misleading.

**Not run:** a real winget install. winget offers 2.53 → 2.55, so the run would turn into an
upgrade with an unattended UAC prompt. Uninstalling Git would break the Hive's own git.

## 5. macOS: not verified

There is no Mac access from NN-WS03. Still open: the CLT route (`xcode-select --install` +
wait + Cancel) on a Mac without Command Line Tools, the Homebrew route, and the shim
detection of `/usr/bin/git` (`GitBinaryState::Stub`).

## 6. Seen along the way (not changed)

* A repository owned by a different uid ("dubious ownership") reads as "no repository" in
  `GitService` (`GitCli::findRepoRoot` is empty), so the panel would offer `git init`. This is
  older than this topic and only showed up through a root/uid mix in the harness.
* The live log shows apt's `\r` progress ("Reading database ... 5%…") as one long line. Cosmetic.

## Handoff

- **Topic / step:** 195, step 4 of 4 (verification). Branch `claude/engine-auto-git-install`.
- **Done:** help test fixed (`tests/test_editor_help.cpp:443`). Harness
  `docs/git-auto-install-e2e/` + `.gitattributes` (LF for its `.sh`). This doc.
- **Verified (how):** builds and tests in sections 1–4 on Windows/MSVC and Linux/GCC.
  Linux end to end on fresh distros: all four cases in section 3, real apt installs, logs
  under `C:\hw195\e2e-linux\`. Windows: locator registry fallback + no-winget fallback
  through the harness.
- **Not verified:** Windows real install (winget + UAC, then the panel without a restart) on a
  machine without git. All of macOS. The editor GUI with a real install on any OS (the UI is
  covered only by `test_git_install_ui.cpp`, headless). The polkit **password dialog** itself
  (a rule allowed it). dnf/pacman/zypper routes (only argv tests).
- **Decisions:** the harness stays outside CMake/he_tests (lesson 286). The help test follows
  step 3's label instead of bringing the old button back.
- **Open for the queen / human:** (1) Windows VM/Sandbox and a Mac without CLT for the
  rest of the verification. (2) Does click-to-install satisfy "the first git-dependent
  operation triggers the install itself"? (3) LFS: after a Linux/macOS install the
  dialog keeps asking for LFS.
- **Next steps:** 1. Windows VM without git: `HorizonEditor` → project with a repo → Install
  Git → UAC → panel shows the repo; also the harness: `git_install_e2e.exe <repo> --install`
  (build line in section 4).
  2. Mac without CLT (tart VM): the same with `--install` → Apple dialog → wait.
  3. Clean up the WSL distros: `wsl --unregister hw195-nogit` / `hw195-nogit2`.
