# EngineContent SFTP Sync — Design

Status: **CP1–CP5 implemented, CP6 (real-server end-to-end + real credentials)
pending** — `src/HE_ContentSync/src/SftpCredentials.cpp` still has placeholder
host/username/password. Once filled in, the flow below works end to end
(verified against the unit-test suite; the actual network round trip has not
been run yet).

## Why

The Editor's shared default-asset library (`EngineContent`, e.g.
`EditorDeps/EngineContent`) is shipped in full with every Editor download. As
that library grows, most of it goes unused by any given project. This feature
lets it be fetched **on demand** from the project's own webhosting, over SFTP —
the only transport that hosting offers — instead of bundled up front.

Scope is deliberately narrow: **only** the Editor's EngineContent library.
**Not** the packaged game's runtime content (`.hpak`) — that already has its
own on-demand streaming (async pak mounting, see `ContentManager::mountPak`)
and stays entirely local-disk-based; SFTP was never wired into it.

## Architecture decisions

**CLI shell-out was considered and rejected.** HorizonSourceControl shells out
to the `git` CLI (`git-CLI statt libgit2`) precisely because libgit2 cannot do
credential helpers or LFS. The same reasoning does not transfer here: the
configured SFTP account uses a password, and OpenSSH's `sftp` CLI cannot answer
a password prompt non-interactively in batch mode. So this links **libssh2**
directly (vendored via `FetchContent`, same pattern as lz4/zstd/Jolt/recast in
the root `CMakeLists.txt`), reusing whichever crypto backend (`OpenSSL` or the
fetched `mbedTLS`) the existing hpak-encryption block already resolved.

**Credentials are hardcoded**, in exactly one file:
`src/HE_ContentSync/src/SftpCredentials.cpp`. This is an explicit, informed
product decision (not an oversight) — see that file's header comment for the
full reasoning and the recommended server-side mitigation (a scoped/chrooted
SFTP account, restricted to the EngineContent publish path).

**No I/O abstraction was added to `HpakReader`.** Every mounted `.hpak` stays
hardwired to local `std::ifstream` I/O; this feature never touches it. Instead,
EngineContent assets are **loose `.hasset` files**, resolved exactly like the
existing default/override roots already are — a third, lowest-priority
resolution tier is simply added alongside them (see below).

## Module: `HE_ContentSync`

Editor-only (mirrors `HE_SourceControl`): not in `HE_Game`'s or
`hc_codegen`'s deploy/copy lists. The whole module — and every call site in
`HE_Editor` — is guarded by `HE_HAVE_LIBSSH2`, defined only when the root
CMakeLists actually resolved a usable libssh2. Absent, the Editor still builds
and runs; EngineContent just stays whatever is present on disk, exactly like
before this feature existed.

| File | Responsibility |
|---|---|
| `SftpCredentials.h/.cpp` | The one place with host/port/user/password/remote-base-path. |
| `SftpClient.h/.cpp` | `sftpTestConnection`/`sftpGetFile`/`sftpPutFile`/`sftpEnsureRemoteDir` — each opens its own TCP+SSH+SFTP session and tears it down (no held connection state, same "reopen per job" principle as `HpakReader`). |
| `SftpProbe.h/.cpp` | Startup connectivity check, mirrors `HE::Sc::GitProbe`. |
| `EngineContentManifest.h/.cpp` | `{path, uuid, contentHash, size}[]`, JSON via nlohmann — `contentHash` is `Hpak::hash64`, the same function incremental pak-writing uses for reuse detection. |
| `EngineContentSync.h/.cpp` | The download queue (`enqueueDownload`/`status()`) — one job in flight at a time, shared by every trigger (Content Browser double-click confirmation, or a scene passively referencing an undownloaded default). |
| `EngineContentPublish.h/.cpp` | Dev-side: scan → diff against the remote manifest → upload changed files → upload the new manifest last. |

## Data flow

**Consumer (every Editor install):**

1. `EditorApplication::startSftpProbe()` (worker thread): connectivity probe →
   `EngineContentSync::refreshManifestBlocking()` → `GlobalState::refreshEngineFolder()`
   with the manifest reshaped into `HE::RemoteEngineAsset` (a tiny `{path, uuid}`
   DTO — see below for why HE_Core needs its own copy of this shape). All of
   this is safe off the main thread: it only touches the filesystem and
   `GlobalState`'s own mutex-guarded tree, never ImGui or `ContentManager`.
2. `OnRender()` (main thread, every frame): consumes an atomic
   "`manifest ready`" flag once, and — this being the one thread allowed to
   mutate it — registers each manifest entry with `ContentManager::registerRemoteAsset()`.
   The same frame also drives `ContentManager::pollAsyncResults(4)`, the
   Editor's per-frame async-arrival drain (previously only the packaged game's
   `GameApplication::OnRender` had one).
3. The Content Browser's Engine folder shows every manifest entry, downloaded
   or not (`HE::File::isRemoteOnly`), via a `mergeManifestInto()` pass in
   `GlobalState::refreshEngineFolder()` — the same tree-merge shape as the
   existing project-override merge, just sourced from the manifest instead of
   a directory scan.
4. Double-clicking a remote-only asset asks for confirmation, then calls
   `EngineContentSync::enqueueDownload(..., DownloadTrigger::Explicit, ...)`.
   A scene silently referencing an undownloaded default resolves through
   `ContentManager::ensureResident()`/`loadAssetAsync()`, which call the same
   queue with `DownloadTrigger::Passive` — **no** confirmation dialog (asking
   once per unresolved reference in a scene would be very disruptive), but
   **the same footer progress widget** either way, so nothing downloads
   invisibly.
5. A finished download lands in `GlobalState::engineContentCacheDir()`
   (`<per-user data dir>/EngineContentCache`) — deliberately **shared across
   every project on this machine**, and **not** next to the Editor executable
   (often unwritable: signed macOS `.app` bundles, `Program Files`). This is
   the third tier `ContentManager::resolveAbsolutePath()` checks for an
   `"Engine/..."` path, after the project override and the shipped default —
   a real shipped file always wins over a cached one.

**Publish (dev-side, gated by `ContentManager::isEngineContentDevMode()`, menu
item "Assets ▸ Publish Engine Content to Server..."):** walks the local
EngineContent tree, hashes + reads the UUID of every `.hasset`, diffs against
the remote `manifest.json`, uploads only what changed, uploads the manifest
**last** (so a consumer fetching mid-publish never sees an entry for a file
that has not landed yet).

## Threading rules (the part most likely to bite a future change)

- `ContentManager`'s own maps (`m_diskRegistry`, `m_remoteAssets`, …) are
  **main-thread only** — same contract `ensureResident()` already documented
  before this feature existed.
- A `materialize` callback (the function `registerRemoteAsset` stores, and
  `EngineContentSync`'s download completion) can fire on **any** thread and
  can outlive the `ContentManager`/`EditorApplication` that created it (a
  network download outlives a closed project). It therefore must never
  capture `this` — see `ContentManager::RemoteReadySink` and
  `EngineContentSync::EngineContentSync()`, both `shared_ptr`-owned sinks, the
  same lifetime pattern `ContentManager::AsyncSink` already established for
  the pak/path async-load jobs.
- `GlobalState::refreshEngineFolder()` **is** safe to call off the main
  thread (it only touches its own mutex-guarded tree and the filesystem) —
  that is what lets `startSftpProbe()`'s worker do the manifest-driven
  re-merge directly instead of bouncing through another queue.

## Why `HE::RemoteEngineAsset` instead of `HE::Cs::EngineContentManifest`

`HE_Core` (`GlobalState`, `ContentManager`) must not depend on `HE_ContentSync`
— that module is editor-only and conditionally absent (`HE_HAVE_LIBSSH2`), and
`HE_Core` is shared with the packaged game. `HE::RemoteEngineAsset`
(`Diagnostics/DiagnosticsStructs.h`) is a two-field DTO `HE_Core` owns itself;
the Editor converts its `HE::Cs::EngineContentManifest` into a vector of these
before calling into `GlobalState`/`ContentManager`. Same reasoning is why the
"materialize a remote asset" hook is a generic `std::function`, not a
`HE::Cs::SftpClient` call baked into `ContentManager`.

## Explicitly out of scope (v1)

- The packaged game / runtime `.hpak` streaming.
- Byte-level download progress (the footer shows "downloading X (n/m)", not a
  byte percentage — `libssh2` can report it, this is a later polish item).
- Resumable/partial downloads (a failed download is a full retry).
- Automatic cache eviction (`EngineContentCache` grows unbounded; deleting the
  folder is the reset).
- A confirmation prompt for passively-triggered downloads (deliberate — see
  the data-flow section above).
