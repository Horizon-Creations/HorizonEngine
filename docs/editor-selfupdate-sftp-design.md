# Editor self-update over SFTP — Design

Status: **Steps 1, 3, 4 and 6 of Thema 186 implemented** (module, second
account, version check, verified download; the takeover mechanism —
`HorizonUpdater`, the swap/rollback/relaunch state machine, extraction,
editor-side orchestration — all with tests that run the real executables as
real processes; the CI matrix's live SFTP leg; and the Editor UI, Help ▸ Check
for Updates…, behind the same unsaved-changes prompt as Quit, shown end to end
on macOS with a real editor, see "The Editor UI"; Schritt 7 repeated that run
on Windows, NN-WS03). **Not done:** the real-account run on the hosting server
(needs its host name, see "Open decisions"), and the end-to-end editor run on
Linux hardware (no Linux device with bees; the script's tar.gz branch has never
run). See "Acceptance" for the verdict against the topic's definition of done.

## What it is

The Editor asks an update server which build is current, compares it with the
running one and, when the server's is newer, downloads the installer for this
platform into a staging directory and verifies it. That is where this module
stops: a verified file waits in `<per-user data dir>/SelfUpdate/` and
`pending.json` says which. Replacing the install, and doing so without losing an
open project or unsaved changes, is the takeover step and is not in here.

## Why a second account, kept apart from HE_ContentSync

`HorizonContentSync` reaches one SFTP account that is compiled into the binary
and scoped to the EngineContent asset library. The self-update uses its own:

* **Scope.** The update account is scoped to the directory holding editor
  builds. One account serving both would force one of the two to be wider than
  it needs to be, and a leak of one would reach the other's files.
* **Cost of a leak.** Whoever can *write* to the update directory can give every
  editor out there code to run. The account the editor holds must therefore be
  **read-only**; publishing a build uses another credential that stays on the
  release machine or in CI.

So the module takes nothing from `HE_ContentSync` except its transport:
`SftpClient`'s functions already take the endpoint as an argument, so
`HorizonSelfUpdate` links `HorizonContentSync` and passes its *own*
`SftpEndpoint`. No file of `src/HE_ContentSync` was changed.

Separation is enforced, not just intended:

* the endpoint is built only from `HE_SELFUPDATE_*` (below); the EngineContent
  account is never a source of values;
* if those variables name the same login (host, port, user) as the compiled-in
  EngineContent account, the config is `Invalid` ("needs its own, separate
  account") and the endpoint is not handed out;
* an unconfigured endpoint never reaches `SftpClient` (its own message would name
  the EngineContent endpoint) and never falls back to the other account;
* the password is scrubbed out of every error text this module returns or logs —
  `HE::Cs::detail::scrub` only knows the EngineContent password.

## Configuration

Process environment, read at run time. Nothing is baked into the binary and
nothing is in a tracked file (this repository is public).

| Variable | | |
|---|---|---|
| `HE_SELFUPDATE_HOST` | required | SFTP host |
| `HE_SELFUPDATE_USER` | required | |
| `HE_SELFUPDATE_PASSWORD` | required | password auth only — `SftpClient`'s limit |
| `HE_SELFUPDATE_PORT` | optional | 1–65535, default 22 |
| `HE_SELFUPDATE_BASEPATH` | optional | directory the tree lives in; empty = the account's root (the chrooted case) |
| `HE_SELFUPDATE_CHANNEL` | optional | `packages` (default) or `preview-packages`; any `[A-Za-z0-9._+-]` name |

None of them set = the feature is off, silently (`ConfigState::NotSet`). Some set
but not usable = `Invalid`, with the missing names listed — never a guess.

A password in the environment is visible to the same user's other processes; it
is the form a CI secret takes anyway. **How a shipped editor on a user's machine
gets its read-only credentials is an open decision** (see below).

## The server layout and the manifest

The update account has two directories at its root, one per channel:

```
packages/              released builds (the default channel)
    manifest.json
    <the files the manifest names>
preview-packages/      preview builds (HE_SELFUPDATE_CHANNEL=preview-packages)
    manifest.json
    ...
```

`<channel>/manifest.json`:

```json
{
  "schema":   1,
  "version":  "0.7.1",
  "codename": "Nimbus",
  "released": "2026-10-20",
  "notes":    "free text",
  "artifacts": [
    { "platform": "windows-x64",
      "path":     "HorizonEditor-windows-x64.zip",
      "sha256":   "<64 hex>",
      "size":     123456789 },
    { "platform": "macos-arm64", "path": "HorizonEditor-0.7.1.dmg", "...": "..." },
    { "platform": "linux-x64",   "path": "HorizonEditor-linux-x64.tar.gz", "...": "..." }
  ]
}
```

* `path` is relative to **the directory the manifest is in**, so a channel
  directory is self-contained: publish by uploading the files, the manifest
  **last** (a client fetching mid-publish then never sees a build that is not
  there yet — the same ordering `EngineContentPublish` uses).
* Platform keys are the names CI already packages under:
  `windows-x64`, `macos-arm64`, `linux-x64` (`currentPlatformKey()`). Another
  host comes out as `<os>-<arch>` and simply finds no artifact.
* Fields this version does not know are ignored, so the format can grow.
  `schema` is bumped only for an *incompatible* change, and an editor that meets
  a higher number refuses rather than guesses — it cannot update itself past
  such a break, so a bump is a decision to make rarely.
* Everything that later becomes a remote path or a local file name is held to an
  allow-list (`isSafeRelativePath`): `[A-Za-z0-9._+-]` segments joined by `/`,
  no leading `/`, no `.`/`..` segment, no `\`, `:`, space or control character,
  no trailing dot. The manifest comes off a network server; a `../` or a drive
  letter in it must never reach a join.

## Flow

```
loadSelfUpdateConfig()  ──►  makeSftpTransport(endpoint)
        │
checkForUpdate()   fetch <channel>/manifest.json (own scratch file, removed again)
        │          parse + validate, compare versions
        ▼
  NotConfigured | UpToDate | RemoteOlder | NoBuildForPlatform | Failed | UpdateAvailable
                                                                          │
downloadUpdate()   already staged and still verifies?  ──► done, nothing fetched
        │          enough disk space?
        │          fetch <channel>/<artifact.path> into <staging>/<version>-<platform>/
        │          size == manifest size?   SHA-256 == manifest sha256?
        │          no ──► delete the directory, earlier pending.json untouched
        ▼
   write pending.json (atomically), remove older staged builds
```

**Policy: newer only.** An update is offered when the remote version is *newer*.
The same version is `UpToDate`; an older remote is reported as `RemoteOlder` (a
rolled-back release is worth knowing about) and never downloaded on its own.
`parseVersion`/`compareVersions` compare numerically — `0.10.0` is newer than
`0.9.0` — and a pre-release (`0.8.0-rc.1`) sorts below its release.

A transient failure (the connection itself: timeout, handshake) is retried up to
three times with a 1 s, 2 s backoff; a missing file, a refused password or an
unsafe path is not.

### Staging

```
<per-user data dir>/SelfUpdate/            (GlobalState::userDataDir(), never next to the exe)
    pending.json                           the ONLY thing the takeover may trust
    0.7.1-macos-arm64/
        .he-selfupdate                     marker: only marked directories are ever pruned
        HorizonEditor-0.7.1.dmg
```

`pending.json`: `{schema, version, platform, file, sha256, size}`, `file` being a
safe path *inside* the staging directory. A file in staging without an entry
there was never verified (an interrupted download, a leftover). The takeover step
calls `readPendingUpdate()` and then `verifyStagedUpdate()` — which hashes again —
right before it acts, so a file damaged or swapped in between is caught.

## What the hash does and does not give

SHA-256 in the manifest proves the file is the one the manifest named, byte for
byte (corruption, a cut-off transfer). Manifest and file come from the same
server over the same SSH session, so this is **integrity, not authenticity**:
anyone who controls the server's update directory, or sits between it and an
editor, can change both together. Two things close that, and neither is in this
step:

1. **A signature** over the manifest (or over each artifact's hash), checked
   against a public key shipped in the editor. The portable choice for the two
   crypto backends the engine already builds (`he_crypto`: OpenSSL or mbedTLS) is
   ECDSA P-256 — mbedTLS 3.6 has no Ed25519. Not added yet.
2. **Host-key pinning.** `SftpClient::Session::open` does not check the server's
   host key at all (it handshakes and logs in). Pinning needs an optional field
   on `SftpEndpoint` and a check in `Session::open`, i.e. a change to
   `HE_ContentSync` — left alone on purpose, since the topic puts that module out
   of scope. Both are additive (an empty pin = today's behaviour).

Until at least the signature exists, the takeover step should be gated behind an
explicit decision, not run silently.

Also from `SftpClient`, worth knowing: password auth only; one TCP+SSH session
per file; a 5 s connect timeout per address but **no read timeout**, so a server
that stops answering mid-transfer holds the worker thread.

## The takeover

The editor cannot replace its own files while it is running — on Windows the
loader holds every DLL and the exe open, and even where the OS would allow it a
process has no business overwriting itself mid-flight. So the swap runs in a
**separate, tiny program, `HorizonUpdater`**, started detached and outliving the
editor:

```
editor (Takeover.cpp: beginTakeover)
    verify the staged build again (readPendingUpdate + verifyStagedUpdate)
    extract it into a SIBLING of the install root        (SelfUpdateInstall.h)
    copy HorizonUpdater to a temp directory
    spawn it detached with an ApplyPlan on its command line, waitPid = own pid
    return — the CALLER quits right after (the editor's own save/discard/
    cancel gate, already in place for Quit/CloseProject/OpenProjectPath,
    is what decides WHETHER to reach this call at all; see below)

HorizonUpdater (SelfUpdateApplyCore.h: applyUpdate), once the editor is gone
    rename installRoot -> backupRoot
    rename payloadRoot -> installRoot            (both on one volume: one rename each)
    copy `preserve`d files (config.json, imgui.ini, the rotated logs) over,
        unless the new build already brought its own
    start the new build, watch it for `healthMs`
    still alive?  Applied.
    exited, or never started?  swap back, relaunch the OLD build.  RolledBack.
    write the result to <stagingRoot>/last-apply.json
```

`preflightTakeover()` is everything from that list that can still be shown to a
user — verification, a writability probe on the install directory's parent,
`HorizonUpdater` actually being there next to the editor — checked **before**
offering "Restart to update?". `beginTakeover()` does the one-way part
(extract, copy, spawn) and returns false on any failure without having touched
`installRoot` at all: the editor keeps running exactly as it was, and the
caller must not quit. Once `beginTakeover()` returns true, quitting is the only
correct next step — `HorizonUpdater` is already waiting on exactly that pid.

**Why the editor validates, not the updater.** By the time `HorizonUpdater`
runs, the editor process is on its way out — there is nobody left to show an
error to. `applyUpdate()` still re-validates everything it can (the payload has
the executable, `backupRoot` does not already exist, the main-exe path has no
`..`) purely as defense in depth, and on every failure **after** the editor is
confirmed gone it relaunches whatever old/original build it can find rather
than leaving the user with nothing running — the one case it does NOT do that
is the editor still being alive past `waitTimeoutMs`, where nothing should be
touched or started at all.

**Extraction uses the OS's own tool, not a bundled library** — each platform
only ever extracts the artifact IT packaged (a Windows editor never sees a
`.tar.gz`), so there is no cross-format need: Windows' bundled `tar.exe`
(bsdtar, reads `.zip`) via `System32`, GNU `tar -xzf` on Linux, and
`hdiutil attach -readonly` + `ditto` (then `hdiutil detach`) for the macOS
`.dmg`. A flat archive (Windows' zip of `package/Editor/*`) and a wrapped one
(Linux's tar with a top-level `Editor/`, a mounted dmg's single `.app`) are
told apart generically: exactly one top-level entry after extraction becomes
the payload root, otherwise the extraction directory itself is.

**Preserved across the swap**, Windows/Linux only (`config.json`, `imgui.ini`,
`HorizonEngine.log` + its three rotated backups): a macOS `.app` bundle is
signed and often read-only, so `GlobalState::setLogFile()` already falls back
to the per-user data directory there instead of writing beside the
executable — nothing of the install itself needs to carry over on that
platform.

**Rename, not copy**, for `installRoot <-> backupRoot <-> payloadRoot`: the
three are siblings on one volume by construction (`Takeover.cpp` places
`payloadRoot`/`backupRoot` next to `installRoot`), so each move is a single
atomic rename, retried for up to `renameRetryMs` against a file a virus
scanner or indexer is still holding — the one thing that does NOT heal with
retrying is a real cross-volume move (`std::errc::cross_device_link`), reported
immediately instead of retried to the deadline.

**Known gap:** a rename blocked for the entire retry window (something holding
a handle open without `FILE_SHARE_DELETE` the whole time) is exercised by the
design but has no automated test — it needs a platform-specific way to hold
such a handle open, deliberately left out of this step to keep the test suite
portable; see "What the next step hangs on".

## The Editor UI (Thema 186 Schritt 6)

**Help ▸ Check for Updates…** — in the ImGui menu bar and in the native macOS
menu (`MacMenuBar` `CheckForUpdates`; on a Mac the ImGui bar is not drawn at
all). Both rows are left out of a build without libssh2, where
`SelfUpdateDialog` compiles to stubs (`HE_HAVE_SELFUPDATE`).

```
Help ▸ Check for Updates…      SelfUpdateDialog::open()      → Session::startCheck   (worker)
  [Download]                   Session::startDownload        → downloadUpdate + preflightTakeover (worker)
  [Restart to Update]          one-slot request, dialog closes   (only when preflight said ok)
EditorUI (renderEditor)        requestGuarded(GuardedAction::InstallUpdate)
  collab host?                 "Update Anyway" / Cancel        (same as Quit)
  scene or asset tab dirty?    Save / Don't Save / Cancel      (the Quit prompt, unchanged)
runGuardedAction               SelfUpdateDialog::beginInstall → Session::install → beginTakeover
                               relaunchArgs = { open .heproj }
  ok                           ctx.quit()  → HorizonUpdater swaps, relaunches with the project
  failed                       editor keeps running, dialog reopens with the reason
```

* `GuardedAction::InstallUpdate` counts as session-ending (`endsSession`), so
  unsaved **asset tabs** are asked about as well as the scene, exactly as for
  Quit. Cancel there abandons the update and changes nothing.
* The open project survives through the relaunch arguments: the new build gets
  the `.heproj` path as its argument, and `EditorApplication`'s launch pick
  (`ProjectLaunchOpen::pickFromArguments`) opens it through
  `ProjectPreflight::requestAtStartup`, like a double-clicked project file. On
  macOS `HorizonUpdater` relaunches through `open -n <bundle> --args <project>`;
  LaunchServices passes both the arguments and the environment through
  (checked: a private `HOME` and `HE_HIDDEN_WINDOW` arrive in the new process).
* On the Project Hub (no project) Restart to Update needs no prompt and runs
  directly, but not while `ProjectPreflight` is opening a project; the request
  then waits and is taken by the guarded path once the project is open.
* `EditorSelfUpdate::Session` (`src/HE_Editor/EditorSelfUpdate.{h,cpp}`) is the
  ImGui-free part: worker thread, phases, an injectable `Backend` (check,
  download, preflight, begin). It is compiled into `he_tests`, so the editor's
  half runs on every CI platform. `SelfUpdateDialog.{h,cpp}` is the window.
* Not done on purpose: no automatic check at start-up and no background
  polling. The account is still an environment variable (Open decision 1), so a
  start-up check would only ever say "not configured" on a user's machine.

**Acceptance driver.** `HE_SELFUPDATE_E2E=save|discard|cancel` plus
`HE_SELFUPDATE_E2E_TRIGGER=<file>`: once the file appears the dialog opens and
presses its own Download and Restart to Update buttons, and the unsaved-changes
prompt guarding `InstallUpdate` is answered with the named button, through the
same code a click runs. Without both variables nothing changes.
`scripts/selfupdate_editor_e2e.py` drives it: a copy of a built editor as the
"installed" build, a 9.9.9 build that is the same editor plus a marker file
(DMG on macOS, tar.gz on Linux, zip on Windows), the read-only test SFTP
account, a private `HOME`, a copy of the project, the editor hidden. It makes
the scene dirty over the Remote Control bridge (`entity_create`), writes the
trigger, then checks the outcome from outside (MCP `scene_info`/`entity_list`
on the old and the relaunched editor, the scene file, the install, HorizonUpdater's
`last-apply.json`). Two traps found building it: (1) into a pipe the editor's
stdout is block-buffered and a killed editor loses its last lines, so the
script reads it through a pseudo-terminal; (2) the editor's own log is
`HorizonEngine.log` beside the executable when that is writable, i.e. inside
the installed bundle, which the swap replaces.

## Files

| | |
|---|---|
| `src/HE_SelfUpdate/CMakeLists.txt` | `HorizonSelfUpdate` SHARED, `HE_SU_API`; links `HorizonContentSync` (PUBLIC — its `SftpEndpoint` is in our header) and `HorizonCore`. Added from the root `CMakeLists.txt` inside `if(HE_HAVE_LIBSSH2)`, right after ContentSync. Editor-only like ContentSync: not in HE_Game's or hc_codegen's deploy lists. |
| `SelfUpdateVersion.h/.cpp` | `parseVersion`, `compareVersions` |
| `SelfUpdateManifest.h/.cpp` | manifest model, validation, `isSafeRelativePath` |
| `SelfUpdateEndpoint.h/.cpp` | `loadSelfUpdateConfig` (env → endpoint), `sameAccount`, `scrubSecret` |
| `SelfUpdateTransport.h/.cpp` | `IUpdateTransport`: `makeSftpTransport` (SftpClient) and `makeLocalDirTransport` (a directory laid out like the server) |
| `SelfUpdateCheck.h/.cpp` | `checkForUpdate`, `downloadUpdate`, `readPendingUpdate`, `verifyStagedUpdate`, `discardStagedUpdates`, `currentPlatformKey`, `runningBuild` |
| `SelfUpdateProcess.h/.cpp` | `spawnDetached`, `processExists`, `waitForProcessExit`, `currentExecutablePath`, UTF-8 path helpers — compiled into BOTH `HorizonSelfUpdate` and `HorizonUpdater` |
| `SelfUpdateApplyPlan.h` | `ApplyPlan` (command-line contract, `toArgs`/`fromArgs`), `ApplyStatus`, `ApplyResult` — header-only, no engine type |
| `SelfUpdateApplyCore.h/.cpp` | `applyUpdate` (the swap/rollback/relaunch state machine), `writeApplyResult`/`readApplyResult`, `appendApplyLog` — compiled into both `HorizonSelfUpdate` (so the next editor start can read the last result) and `HorizonUpdater` |
| `SelfUpdateInstall.h/.cpp` | `extractStagedBuild` (OS tool per platform), `mainExecutableRelativePath`, `installRootForRunningBuild` |
| `Takeover.h/.cpp` | `preflightTakeover`, `beginTakeover` — the editor-side entry point, called from `EditorSelfUpdate::Session` |
| `src/HE_Editor/EditorSelfUpdate.h/.cpp` | `Session` (check / download / install on a worker, injectable `Backend`), `relaunchArgsFor` — ImGui-free, also compiled into `he_tests` |
| `src/HE_Editor/SelfUpdateDialog.h/.cpp` | Help ▸ Check for Updates… window, the one-slot install request, `beginInstall`, the `HE_SELFUPDATE_E2E` acceptance driver; stubs without `HE_HAVE_SELFUPDATE` |
| `src/HE_Editor/EditorUI.cpp` | `GuardedAction::InstallUpdate` (enum, `runGuardedAction`, `endsSession`, collab wording), menu row, request poll in `renderEditor`, direct path on the Hub |
| `tests/test_editor_selfupdate.cpp` | 6 cases: the relaunch argument reopens the project through `pickFromArguments`; check and download never begin the takeover; a failed begin leaves staging intact and does not quit; preflight blocker hides Restart; damaged download; no account |
| `scripts/selfupdate_editor_e2e.py` | the end-to-end run with a real editor, `--mode save|discard|cancel` |
| `updater/main.cpp` → **`HorizonUpdater`** | the separate executable `Takeover.cpp` spawns; `HE_SU_STANDALONE`, links no engine library, deployed beside `HorizonEditor` (`he_deploy_dll`, and on macOS `package_macos.sh` copies it into `Contents/MacOS`) |
| `tests/test_selfupdate.cpp` | 17 cases; `ctest -R test_selfupdate` |
| `tests/test_selfupdate_apply.cpp` | `HorizonUpdater` run as a real child process against `tests/helpers/fake_editor.cpp` (`he_fake_editor`, a real process standing in for both "the editor being waited on" and "the build being relaunched"): `ApplyPlan` CLI round trip, happy swap (preserves `config.json`, relaunch args reach the new build), the editor outliving the wait timeout (nothing touched), a new build that dies on arrival (rollback + the old build relaunched) |
| `scripts/selfupdate_test_sftp_server.py` | read-only paramiko SFTP server with password login, for the live test and the CI matrix |
| `scripts/selfupdate_live_ci_check.py` | the CI-side orchestrator (Thema 186 Schritt 4): writes a one-build manifest+artifact fixture, starts the server above with a generated password, points `HE_SELFUPDATE_*` at it and runs `ctest -R ^test_selfupdate$` |
| `HE::Log::Cat::SelfUpdate` | log category (`HE_LOG=SelfUpdate=Debug`) |

Windows export rule (see the `HE_API` rules in the Windows CI notes): the DLL
uses explicit exports, so every free function carries `HE_SU_API`, **and** every
struct that has a member function defined in a `.cpp` is exported as a whole
type (`struct HE_SU_API Version`, `UpdateArtifact`, `UpdateManifest`,
`SelfUpdateConfig`). A member of a non-exported type links on macOS and Linux
and is an unresolved external for every consumer (the tests included) on MSVC.
New types with only inline members need nothing.

The transport being an interface is what makes the rest testable without a
network, and lets the CI matrix prove the whole path on a runner before (or
without) a real server.

## Verification

* **Unit / logic** (`test_selfupdate.cpp`, all in the default run): version
  ordering incl. `0.10.0 > 0.9.0` and pre-releases; manifest round trip,
  unknown-field tolerance, and 23 kinds of unsafe or incomplete manifest rejected;
  env → endpoint incl. "setting only the EngineContent variables does not light
  this up"; same-account refusal; password scrubbing; check outcomes against a
  directory-backed server (newer / same / older / pre-release / no platform build
  / missing or corrupt manifest / retry vs. no retry); download verify (happy
  path, already staged, staged file damaged, server file swapped with the earlier
  staging left intact, truncated, missing, dropped connection, newer replaces
  older, `pending.json` pointing out of staging, discard leaves foreign
  directories alone); the SFTP transport refusing an unconfigured account and an
  unsafe path without leaking the password.
* **Live** (opt-in, `HE_SELFUPDATE_LIVE_TEST=1`; otherwise skipped, and reported
  as skipped): real libssh2 + `SftpClient` + password login against a real SFTP
  server: manifest check, 3 MB download in many chunks, size + SHA-256, the same
  version afterwards reading as up to date, and a wrong password failing with an
  authentication error that does not contain the password.

  ```
  pip install paramiko
  HE_SELFUPDATE_PASSWORD=<test password> python3 scripts/selfupdate_test_sftp_server.py \
      --root <dir with packages/manifest.json + the artifact> --port 22231 &
  HE_SELFUPDATE_LIVE_TEST=1 HE_SELFUPDATE_HOST=127.0.0.1 HE_SELFUPDATE_PORT=22231 \
  HE_SELFUPDATE_USER=editor-ro HE_SELFUPDATE_PASSWORD=<test password> \
      he_tests -tc="selfupdate*"
  ```

  Run on macOS arm64 for step 1: 17/17 test cases, 295 assertions, 0 skipped;
  the server log shows only reads. The artifact must be built for the platform
  the test runs on (the manifest needs a `macos-arm64` / `linux-x64` /
  `windows-x64` entry accordingly).

## Acceptance (Thema 186 Schritt 5, 2026-10-12)

Measured against the topic's definition of done: self-update over SFTP
demonstrably works on Windows, macOS and Linux without losing editor state,
and the GitHub pipeline checks it through a platform matrix. Evidence is CI
run 38255921746 (commit 067fdf08) unless stated otherwise; all three of its
jobs ran on **GitHub-hosted** runners (`macos-26-arm64`, `ubuntu-24.04`,
`windows-2025-vs2026`), not on NN-WS03 or another real device.

| Criterion | State | Evidence |
|---|---|---|
| Second SFTP account, kept apart from HE_ContentSync | done | `test_selfupdate` ("the account comes from HE_SELFUPDATE_* and nothing else", "the same login as the EngineContent account is not a second account") passed on all three runners |
| Manifest/version check + SHA-256-verified download over a real SFTP session | done | live leg on all three runners: `live: offered 9.9.9 (3145728 bytes)`, doctest 17 passed = 15 offline + 2 live (round trip, wrong password refused) |
| Takeover (swap, relaunch, rollback) on every platform | done at process level | `test_selfupdate_apply` passed on macOS (6.9 s), Linux (6.2 s), Windows (6.4 s): real `HorizonUpdater` + `he_fake_editor` child processes, happy swap keeps config, timeout aborts untouched, crash on arrival rolls back |
| Helper ships with the editor | Windows + Linux done, macOS open | `package/Editor/HorizonUpdater[.exe]` present on Windows and Linux; the macOS DMG check never saw a DMG in that run (the packaging step was skipped after the live check failed), so DMG content is only proven by the next run |
| No editor state lost | **macOS + Windows done (real editor); Linux: logic in CI only** | Schritt 6 (macOS) and Schritt 7 (Windows, NN-WS03), see the tables below; `test_editor_selfupdate` runs in the CI matrix on all three; the end-to-end editor run on Linux is open |
| Real update account on the hosting server | **open** | host name never supplied (Hive request #49); every live run so far is against `selfupdate_test_sftp_server.py` |
| CI matrix checks it automatically | done | `.github/workflows/ci.yml` (`claude.yml` is only the `@claude` bot) |

**Both reds in run 38255921746 were defects in the step-4 checks, not in the
product.** (1) `selfupdate_live_ci_check.py` demanded doctest's `0 skipped`,
which a `--source-file`-filtered single binary never prints (~5000 other cases
count as skipped); it now compares against an offline baseline run, see
"Verification" above. Checked locally against the real ctest output of all
three runners and end to end with real `ctest`, the real test SFTP server and
a stand-in `he_tests` (green when the env arrives, red with "passed 15 -> 15"
when it does not). (2) The DMG check ran with only `!cancelled()` and blamed
`package_macos.sh` for a DMG that had never been built; it is now gated on
`steps.dmg.outcome == 'success'` and says so separately when no DMG exists.

### Schritt 6: end to end on macOS (2026-10-12, commit of this step)

`scripts/selfupdate_editor_e2e.py --install-src out/dmg_staging/HorizonEditor.app`
on a MacBook Air (M-series, macOS 27), Release build of this branch packaged
by `package_macos.sh`, project a copy of HorizonTutorial, the editor hidden:

| Mode | Checks | What it showed |
|---|---|---|
| `cancel` | 9/9 | prompt raised ("scene dirty, 0 asset tab(s)"), Cancel → editor still running, `scene_info.dirty` still true, the unsaved entity still in the scene, HorizonUpdater never started, install and scene file unchanged |
| `save` | 13/13 | prompt → Save wrote the scene, HorizonUpdater started, old editor exited 0, `last-apply.json` `applied` 0.7.0 → 9.9.9, the bundle holds the 9.9.9 marker, the relaunched editor runs from the swapped bundle with the `.heproj` argument, logs "opening … (handed over at launch)", has HorizonTutorial open, clean, with the saved entity in it |
| `discard` | 13/13 | the same swap and relaunch; the scene file on disk unchanged and the discarded entity absent in the relaunched editor (the user's choice, honoured) |

### Schritt 7: end to end on Windows (2026-10-12)

The same script on NN-WS03 (Windows 11, Ryzen 9 9950X, RTX 4070), a Release
build of this branch (commit 443a4285 plus the script fixes below) configured
into its own tree with its own `DEPLOY_DIR`; `--install-src` the flat deploy
`<DEPLOY_DIR>/Editor` (what CI zips as `package/Editor`), project a copy of
HorizonTutorial, the editor hidden, OpenGL (the Windows default RHI), the test
SFTP account from a paramiko venv:

| Mode | Checks | What it showed |
|---|---|---|
| `cancel` | 9/9 | prompt raised, Cancel → editor still running, `scene_info.dirty` still true, the unsaved entity still in the scene, HorizonUpdater never started, install and scene file unchanged |
| `save` | 13/13 | Check → Download (47.9 MB zip, SHA-256 verified) → Restart to Update → prompt → Save wrote the scene; old editor exited 0; `last-apply.json` `applied` 0.7.0 → 9.9.9, payload `…\payload\extract` (the flat-zip branch of `collapseSingleTopLevelDir`); the install holds the 9.9.9 marker; the relaunched editor's `Win32_Process` command line is `<install>\HorizonEditor.exe <project>.heproj`, it has HorizonTutorial open, clean, with the saved entity in it |
| `discard` | 13/13 | the same swap and relaunch; the scene file on disk unchanged and the discarded entity absent in the relaunched editor |

The Windows branch of the script had never run and needed fixes before it
could: `os.kill(pid, 0)` is `TerminateProcess` on Windows (the liveness probe
killed what it probed) and `signal.SIGKILL` does not exist there (now ctypes
`GetExitCodeProcess` / `taskkill`); the editor's stdout redirected into a file
is block buffered on Windows, so the cancel run would have waited for a line
that only reaches the file at exit (the line was in `HorizonEngine.log`, 0 times
in stdout) — the script now also reads `HorizonEngine.log` / `.1.log` beside the
exe, opening and closing it on each read so no handle blocks HorizonUpdater's
rename, and only after `last-apply.json` exists in save/discard; the fixture zip
was wrapped in `Editor/` while the shipped one is flat (`Compress-Archive
package/Editor/*`), now flat; the relaunched editor's command line was only
checked through `ps` (two checks silently skipped on Windows), now
`Win32_Process`; `HE_CONFIG_DIR` pins both editors to the private config (the
relaunched one runs with the install as working directory, where a
`config.json` would win) and `HE_NET_LOOPBACK_ONLY=1` keeps the new exe path
free of a firewall prompt; run leftovers (`HorizonEngine*.log`, `config.json`,
`imgui.ini`) are not copied out of the source deploy.

**Found on the way, not changed (product decision):** nothing removes the work
directory `<install parent>/.heselfupdate-<version>-<suffix>/` after a
successful update. Its `backup/` is the complete previous install (118 MB for
this deploy), so every update leaves one more full copy beside the install.
Keeping the last one is a manual rollback; keeping all of them is a leak. To
decide: delete older ones on the next start of the new build, or keep exactly
one.

Not shown yet: the same run on Linux (the tar.gz branch of the script is
unrun; NN-WS03 has no WSL distribution and the Linux device NN-RP01 runs no
bees), and against the real account (host name still missing).

## Open decisions (need the human)

1. **Where does a shipped editor get its credentials?** Env vars suit CI and a
   developer; a user who installed the DMG has none. The options are a
   build-time definition the way ContentSync does it (read-only account, so a
   leak is "someone can download builds", not "someone can publish"), or a
   settings entry. Not decided here; the loader takes an `EnvLookup`, so another
   source is a small addition.
2. **Signature and host-key pinning** (above) before the takeover runs
   unattended.
3. **Same-version rebuilds.** Versions come from `project(VERSION)`; a new build
   of the same `0.7.0` is "up to date". Updating between builds of one version
   needs a build number in the manifest and in the binary.
4. **The real account.** The two directories (`packages/`, `preview-packages/`)
   are assumed to hold a `manifest.json` in the format above; what is on the
   server today, and who writes the manifest on release, is for the publishing
   step (upload the files, the manifest last).

## What the next step hangs on

* **The Editor UI — done in Schritt 6**, end to end on macOS (Schritt 6) and
  Windows (Schritt 7). What is left of it: run `scripts/selfupdate_editor_e2e.py`
  on Linux (a tar.gz of the flat deploy; that branch has never run, it needs a
  Linux machine with a display or Xvfb, a build of this branch and a paramiko
  venv), the cleanup of `.heselfupdate-*` above, and, once Linux is green,
  consider a CI leg after the packaging step (~1 min per mode).
* **The known gap above** (a rename blocked for the whole retry window) has no
  test; it needs a platform-specific way to hold a directory-internal file
  handle open without `FILE_SHARE_DELETE` for the duration.
* **Step 4 (CI) — done.** The matrix is `.github/workflows/ci.yml` — **not**
  `claude.yml`, which is only the `@claude` comment bot. `ci.yml` already ran
  `ctest`, so `test_selfupdate` and `test_selfupdate_apply` (the offline cases)
  were already exercised on all three platforms before this step; what it adds
  is the one thing that was not covered yet — the two cases gated behind
  `HE_SELFUPDATE_LIVE_TEST=1`, a real `SftpClient`/libssh2 session against a
  real server, plus a check that the takeover helper actually ships.

  Two new steps, right after "Run tests" in the `build-and-test` matrix job
  (not the `vulkan-lavapipe` job — it never builds `he_tests`), one per shell
  because NN-WS03 (self-hosted Windows) is not guaranteed a `bash` (`ci.yml`
  says so twice already, at the Vulkan-assert and compiler-cache steps): a
  `shell: pwsh` step for `runner.os == 'Windows'` and a `shell: bash` step for
  everything else, both gated on `steps.build.outcome == 'success'` (the
  `Build` step now has `id: build`) so a build failure shows up once, not a
  second time as a confusing "executable not found". Each installs `paramiko`
  with `pip install --target` into `runner.temp` — not the ambient
  interpreter, which on NN-WS03 is shared with up to eight other bees and
  would race any other install happening at the same moment — then runs
  `scripts/selfupdate_live_ci_check.py --build-dir build`, which:
  1. writes a one-build manifest + artifact fixture for whichever platform the
     job is;
  2. starts `scripts/selfupdate_test_sftp_server.py` as a throwaway, read-only
     account on 127.0.0.1 with a password generated on the fly (never written
     to a file, never printed) — no secret involved, so this runs on every PR,
     forks included, unlike the real update account (still open, "Open
     decisions" #4);
  3. runs `ctest -R ^test_selfupdate$ -V --no-tests=error` twice — first with
     every `HE_SELFUPDATE_*` stripped (the offline baseline, live cases
     skipped), then live against the server — parses doctest's own `test
     cases: N | M passed | F failed | S skipped` summary out of both, and fails
     unless the live run has exactly 2 more passed and 2 fewer skipped cases
     than the baseline and the round trip's `live: offered 9.9.9` MESSAGE is
     in its output. This is not redundant with ctest's exit code: a
     `doctest::skip(...)`-gated case that never actually ran because the
     live-test environment did not reach the child process is itself a ctest
     PASS, which would otherwise let a wiring regression report green while
     proving nothing. (Step 4 first insisted on `S == 0`, which can never
     hold: he_tests is one binary, ctest filters it with
     `--source-file=*test_selfupdate.cpp`, and doctest counts every case that
     filter excludes, ~5000, as skipped too. CI run 38255921746 went red on
     all three platforms on that alone, with both live cases passed: 17
     passed = 15 offline + 2 live.)

  Then, right before/after the three platforms' packaging steps: "Check
  HorizonUpdater is deployed beside the editor" (Windows, Linux — checks the
  raw `package/Editor/HorizonUpdater[.exe]`, since `he_deploy_dll` puts it
  there as part of the ordinary build, not a packaging step that could drop
  it) and "Check HorizonUpdater is inside the macOS package (DMG)" (mounts the
  real `.dmg` read-only, the same way `SelfUpdateInstall.cpp` extracts one on
  a user's machine, and looks in `Contents/MacOS`) — macOS is the one platform
  where this can actually go missing, because `package_macos.sh` only *warns*
  and keeps going when `HorizonUpdater` isn't where it expects it (see the
  kontext lesson on this file), on purpose, so a self-update regression there
  does not block every other DMG build; this CI check is what turns that warn
  into a hard failure.

  **Trap found building this, now fixed:** the `python3`-then-`python`
  fallback other `shell: bash` steps use (`command -v python3 || command -v
  python`) is not reliable on a plain Windows PC — NN-WS03 included.
  `command -v python3` resolves to the Windows "App execution alias" stub
  (`%LOCALAPPDATA%\Microsoft\WindowsApps\python3.exe`), which exists as a file
  (so `command -v` accepts it) but exits 49 with a Microsoft-Store nag the
  moment it is actually run — `python` (the real interpreter) is never
  reached. The existing fallback in the "Build the two app runtimes" step
  never hit this because it is push-only, and on Windows a push always routes
  to the hosted `windows-latest` runner, never to NN-WS03. The fix ended up
  being the OS split above, not a cleverer fallback: the `pwsh` branch tries
  `python` *before* `python3` (same order as the existing ".heproj
  registration (Windows)" step), which the stub never shadows; the `bash`
  branch keeps the ordinary `python3`-first order, correct on macOS/Linux,
  which have no such stub. See the Hive lessons on this topic (one superseded
  by a correction) for the reproduction and the wrong first fix.

  **Local verification for this step** (no engine build available/affordable
  here): `scripts/selfupdate_live_ci_check.py`'s own logic — fixture creation,
  the server round trip, the thread-backed `wait_for_ready` (including its
  timeout path against a process that never prints), and the doctest
  skip-count parser (including the "`10 skipped`" vs. "`0 skipped`" substring
  trap) — was smoke-tested directly against a real
  `scripts/selfupdate_test_sftp_server.py` process and a real `paramiko` SFTP
  client. The `ctest`-running half is the existing, already-proven `ctest
  --test-dir build` call with a `-R` filter, `-V` and extra environment added
  — not independently re-verified end-to-end locally; that is what the pushed
  branch's CI run now proves on real Windows/macOS/Linux runners (step 5 is
  where a human-visible verdict on that run belongs). The two "HorizonUpdater
  shipped" checks are untested against a real package/DMG for the same reason.
* **A Debug build of `he_tests` on Windows currently fails** regardless of this
  topic: `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:469` and
  `:755` hit MSVC's string-literal length limit (C2026) in a `Debug`
  configuration specifically — `Release` compiles the same file. Local
  verification for this step used a Release configuration; see the topic's
  lessons for the pointer.
