# Git auto-install, step 2: detection (is git there, which version, which install route)

Topic 195, step 2. Builds on `docs/git-auto-install-callsites-2026-10-13.md` (step 1).
Detection only. Nothing installs, nothing asks the user.

## What is there now

`src/HE_SourceControl/include/SourceControl/GitLocator.h` + `src/GitLocator.cpp` (namespace `HE::Sc`):

| API | What it does |
|---|---|
| `gitExecutable()` | Absolute path of the git to launch, cached behind a mutex. **Never spawns a process** (it is reached from the frame thread). Windows: `Proc::which("git")` (only an `.exe` counts, CreateProcess cannot run `git.cmd`), then the **registry** PATH as it is *now* (HKLM Session Manager\Environment, then HKCU Environment, `ExpandEnvironmentStringsW`), then `%ProgramW6432%/%ProgramFiles%/%ProgramFiles(x86)%\Git\cmd` and `%LOCALAPPDATA%\Programs\Git\cmd`. POSIX: `Proc::which` (Homebrew prefixes added by `augmentToolPath`) |
| `invalidateGitLocation()` | Drops the cache. Called by `probeGit()` (= Recheck), and on every `launchFailed` |
| `gitLaunchPath()` | Located path, or the bare `"git"` when nothing was located, so nothing regresses if the OS knows a git the locator does not |
| `splitPathList`, `findExecutableInDirs` | Pure PATH helpers (quotes, empty entries, dirs named `git`, POSIX exec bit) |
| `classifyGitBinary(platform, found, developerToolsPresent)` | `NotFound / Stub / Runnable`. `Stub` = macOS `/usr/bin/git` without CLT/Xcode |
| `planGitInstall(platform, toolExists, isRoot)` | The route table, pure. `detectGitInstallPlan()` = same for this machine (`Proc::which`, `geteuid`) |

Routes (`GitInstallPlan.route`, `available` preferred first, `elevation`, `installCommand`, `manualCommand`, `bringsLfs`, `reason`):

| Platform | Preferred → fallback | Elevation | LFS along |
|---|---|---|---|
| Windows | `Winget` (`winget install --id Git.Git -e --source winget`) → `DownloadInstaller` (always listed) | `OsPrompt` (UAC) | yes |
| macOS | `Homebrew` (`brew install git`) → `XcodeCommandLineTools` (`xcode-select --install`) → `None` | `NotNeeded` / `OsPrompt` | no |
| Linux | first of `apt-get`, `dnf`, `pacman`, `zypper` | `AlreadyRoot`, else `Pkexec`, else **`Unavailable`** | no |

`Unavailable` and `None` are the two "fallback hint" cases (`canInstallAutomatically()` is false);
`manualCommand` then carries the terminal line, with `sudo` on every `&&` part.

Wiring:
- All six launch lines now use `gitLaunchPath()`: `GitCli.cpp` `runImpl` and the three credential
  calls (now via `runCredential`, which invalidates and reports "git could not be started"),
  `GitProbe.cpp` `runCapture`, `EditorApplication.cpp` `applyGitIdentity`.
- `GitResult::gitMissing` is set by `runImpl` on `launchFailed`.
- `GitProbe` gained `gitBinary` and `install`. `probeGit()` invalidates, locates, classifies,
  does **not** run a macOS stub (it may pop Apple's installer), fills `install` on every probe and
  adds the route to `detail`. `gitFound` still means "`git --version` ran".

## Tests

`tests/test_git_locator.cpp` (10 cases): PATH splitting, first-hit order, decoys, POSIX exec bit,
stub classifier, the full route table per platform (every Linux manager × pkexec/root/neither),
route names, and a machine smoke test (locator ⊇ `Proc::which`, probe agrees, prints the route).

## Handoff

- **Topic / step:** 195, step 2 of 4. Branch `claude/engine-auto-git-install`.
- **Verified (NN-WS03, Windows, Release, `C:/hw195`):** `he_tests --source-file=*test_git_locator*,*test_sourcecontrol_probe*,*test_git_roundtrip*,*test_git_panel_ops*,*test_git_status_parse*`
  → 79/79 cases pass (round trips now launch git through the locator). With every `\Git\` entry
  removed from PATH (`Get-Command git` fails), the locator still returns
  `C:\Program Files\Git\cmd\git.exe` and the probe runs `--version` through it, which is the
  stale-PATH-after-install case. winget is detected through the WindowsApps alias (`route: winget`).
  `HorizonEditor` builds. g++ (WSL) `-fsyntax-only -Wall -Wextra` is clean for the four changed/new
  sources.
- **Not verified:** macOS and Linux at runtime (CI only). Whether running the macOS `/usr/bin/git`
  shim really pops the CLT dialog: the code avoids running it, but this was not observed on a Mac.
  Registry-only lookup with git also missing from `%ProgramFiles%` was not isolated.
- **Decisions:** no Git installer is bundled with the engine (grep found none), so the Windows
  fallback is `DownloadInstaller`, not "bundled". Homebrew is preferred over the CLT on macOS
  (no dialog, can add git-lfs afterwards). The locator is file-lookup only; the macOS
  `xcode-select -p` check lives in `GitProbe` (worker thread). `gitExecutable()` still returns the
  macOS stub path, so existing launches behave as before; only the probe treats it as missing.
- **Open / for step 3:** `GitService` Kind::Open (`GitService.cpp:384`) still reads an empty
  `findRepoRoot` as "not a repo": `findRepoRoot` returns a path, not a `GitResult`, so it needs
  `gitMissing` threaded through (or a `gitExecutable()` check first). Git LFS: `bringsLfs` says
  which routes carry it, and the queen still has to decide whether LFS gets installed too.
- **Next steps:** 1. `EditorApplication::startGitInstall()` that runs `probe.install`
  (winget / download / brew / xcode-select / pkexec + `installCommand`), modelled on
  `startToolchainInstall`. Then `invalidateGitLocation()` + `probeGit()`. 2. A dialog for
  `canInstallAutomatically() == false` that shows `manualCommand` + `reason`. 3. Hook the UI gates
  from step 1 §2 and a `gitMissing` result into it.
