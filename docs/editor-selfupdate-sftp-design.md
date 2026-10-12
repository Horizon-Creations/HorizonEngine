# Editor self-update over SFTP — Design

Status: **Step 1 of Thema 186 implemented** (module, second account, version
check, verified download, tests, live round trip against a local SFTP server).
**Not done:** taking a staged build over on Windows / macOS / Linux without losing
an open project (step 2), the CI matrix (step 3), the real-account run on the
hosting server (needs its host name, see "Open decisions"), and anything in the
Editor UI. Nothing in `HorizonEditor` links this module yet.

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

## Files

| | |
|---|---|
| `src/HE_SelfUpdate/CMakeLists.txt` | `HorizonSelfUpdate` SHARED, `HE_SU_API`; links `HorizonContentSync` (PUBLIC — its `SftpEndpoint` is in our header) and `HorizonCore`. Added from the root `CMakeLists.txt` inside `if(HE_HAVE_LIBSSH2)`, right after ContentSync. Editor-only like ContentSync: not in HE_Game's or hc_codegen's deploy lists. |
| `SelfUpdateVersion.h/.cpp` | `parseVersion`, `compareVersions` |
| `SelfUpdateManifest.h/.cpp` | manifest model, validation, `isSafeRelativePath` |
| `SelfUpdateEndpoint.h/.cpp` | `loadSelfUpdateConfig` (env → endpoint), `sameAccount`, `scrubSecret` |
| `SelfUpdateTransport.h/.cpp` | `IUpdateTransport`: `makeSftpTransport` (SftpClient) and `makeLocalDirTransport` (a directory laid out like the server) |
| `SelfUpdateCheck.h/.cpp` | `checkForUpdate`, `downloadUpdate`, `readPendingUpdate`, `verifyStagedUpdate`, `discardStagedUpdates`, `currentPlatformKey`, `runningBuild` |
| `tests/test_selfupdate.cpp` | 17 cases; `ctest -R test_selfupdate` |
| `scripts/selfupdate_test_sftp_server.py` | read-only paramiko SFTP server with password login, for the live test and the CI matrix |
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

## What the next steps hang on

* **Step 2 (takeover):** `readPendingUpdate()` + `verifyStagedUpdate()` is the
  entry. Attach the "unsaved changes" gate to `EditorApplication.cpp` and the
  save checks in `ProjectPreflight.cpp`; when the Editor starts linking
  `HorizonSelfUpdate`, check that the macOS bundle step, the Windows zip and the
  Linux tar pick the new library up — none of them names ContentSync, they take
  what is in the deploy directory, but verify it.
* **Step 3 (CI):** the matrix is `.github/workflows/ci.yml` — **not**
  `claude.yml`, which is only the `@claude` comment bot. `ci.yml` already runs
  `ctest`, so `test_selfupdate` runs on all three platforms as soon as this lands;
  the live leg needs `pip install paramiko`, the server script above and a
  platform-matching artifact. On Windows, `he_tests` now needs
  `HorizonSelfUpdate.dll` and `HorizonContentSync.dll` beside it (copied in
  `tests/CMakeLists.txt`). `ci.yml`'s `concurrency: ci-<branch>,
  cancel-in-progress` applies: finish before the one push.
