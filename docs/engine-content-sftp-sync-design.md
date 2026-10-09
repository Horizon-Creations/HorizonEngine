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

## What rides along: the weather sounds (Thema 167)

`EditorDeps/EngineContent/Audio/Weather/{Rain,Wind,Snow,Storm,Thunder}.hasset` are
committed like the meshes and materials (generated by `weather_sound_gen`, UUID
block `0x500`, PCM16, about 1 MB together), so every editor install has them
locally and nothing is fetched on demand. What was checked for the sync and the
export, and where it is pinned (`tests/test_engine_weather_sounds.cpp`):

- **Publish scan.** `publishEngineContentBlocking` walks the tree recursively for
  every `.hasset` whose META UUID it can read and that is not the null UUID. The
  five files are listed with their UUIDs and a content hash; a first "Publish
  Engine Content" uploads five files and about 1 MB. The test reproduces that read
  on the real files. Nothing was published: sending files to the server is the
  dev's call, not part of a build or a test run.
- **Pak export.** `ExportDialogPanel` always hands the exporter the whole engine
  content root and `HpakWriter::addDirectories` packs every `.hasset` of every
  root, so a shipped game carries them whether or not a scene names them. That is
  why `SceneSystems::collectAssetRefs` must not list the defaults. The test
  exports, mounts the pak with no engine root and runs the weather on it.
- **The licence note** (`Audio/LICENSE-WeatherSounds.txt`) is not a `.hasset`: the
  publish scan, the registry and the pak all skip it; the Content Browser shows it
  as a loose file beside the sounds.

## CI credentials across platforms (Thema 167 step 4)

`.github/workflows/ci.yml` passes the `HE_ENGINE_CONTENT_HOST/USER/PASSWORD`
repository secrets as environment variables (never `-D`, see the comment at
`src/HE_ContentSync/CMakeLists.txt:59`) into whichever `Configure` step builds a
target that links `HorizonContentSync`. Two different questions, checked
separately, because the configure-time log line only answers the first one:

1. **Do the credentials reach the compile definitions** of whatever links
   `HorizonContentSync`? Witnessed yes on macOS, Linux and Windows, and since
   the fix below also on lavapipe: the run dispatched at the end of step 4
   (`.../actions/runs/37823613186`, commit 94527920) logs
   `HorizonContentSync: EngineContent endpoint ***@***:22` in all four jobs
   (macOS, Linux, Windows, Linux · Vulkan (lavapipe)), and the three
   `HE_ENGINE_CONTENT_*` variables show up (masked) in the lavapipe job's
   `Configure` step environment. Witnessed in the step-5 verification, from
   the finished job logs. In that run Windows again ran on the hosted
   `windows-latest` runner (point 5 still stands). The run itself ended
   `failure` on two tests only, `test_editor_help` and `editor_help_audit`,
   on all three matrix jobs: they come from `7cef67a9` (Import to Project),
   which is on the `release/0.7.0` base (the audit lists the same four open
   controls on an unchanged `release/0.7.0` tree), not from the weather
   change. The fix `978ac7e1` (topic 176) turns both green; step 6 took it
   over onto this branch as `256f20fe` (cherry-pick -x), because it was not yet
   on `release/0.7.0`. Locally afterwards: `editor_help_audit.py --check` rc 0
   (interface 180/180), `test_editor_help` and `editor_help_audit` pass, full
   ctest 249/249 (2 skipped, `test_material_graph` left out as unrelated).
   Verified by reading completed runs' job logs directly
   (`gh api .../actions/jobs/<id>/logs`, grepped for the line below — not taken
   on faith from an earlier survey):
   - A green push run on `main`, 2026-10-08 08:56
     (`https://github.com/Horizon-Creations/HorizonEngine/actions/runs/37753155172`):
     macOS, Linux and Windows (all hosted runners) each log
     `HorizonContentSync: EngineContent endpoint ***@***:22` once, from the main
     `Configure (<platform>)` step.
   - A push run on `release/0.7.0`, 2026-10-08 14:50
     (`.../actions/runs/37795709487`, overall `failure` on an unrelated build
     break, but every `Configure` step itself succeeded): same line on macOS,
     Linux, Windows; the **lavapipe** job instead logs
     `HorizonContentSync: EngineContent endpoint not configured — the remote
     library stays off` (job conclusion: `success` — no credentials is not an
     error, the build just proceeds with the feature off) — the negative
     control for the gap this step closes (its `Configure` step had no `env:`
     block; fixed by adding the same three lines the three matrix jobs already
     had). This is also the witnessed instance of point 6's "no secrets" case
     below, not a hypothetical.
2. **Does the sync actually run** — does any CI job make a live SFTP connection
   or fetch an asset? **No, on every job, regardless of credentials.** Traced
   through the code, not just inferred from logs. Two ways a sync can start:
   - **Automatically at startup**: `EditorApplication::startSftpProbe()`
     (`EditorApplication.cpp:2074`), called once from `OnInit()` — but guarded
     by `if (m_dumpPath.empty())` (`:1967`), and `m_dumpPath` is set whenever
     `HE_DUMP_PATH` is in the environment (`:818`). No CI job ever launches
     `HorizonEditor` without `HE_DUMP_PATH` set: the only job that runs the
     real binary at all is Linux · Vulkan (lavapipe), through
     `scripts/he_vk_imagetests.py`, which sets `HE_DUMP_PATH` for every single
     invocation (headless frame dump, one BMP per shot) — so `startSftpProbe()`,
     along with the toolchain/git/router probes next to it, never fires there
     either. The matrix jobs never launch the editor binary at all — checked
     every `add_test()` in `tests/CMakeLists.txt`: `he_tests` itself doesn't
     compile `EditorApplication.cpp` (no `ContentSync` doctest exists to need
     it), `editor_help_audit` and `test_he_mcp.py` read source/fake a socket
     without an editor process, and the `runtime_size*` cases only stat files
     on disk.
   - **Manually, from a button**: `refreshEngineContentManifest()`
     (`:2163`, which is what encloses the `refreshManifestBlocking()` call at
     `:2174`) and `publishEngineContentBlocking` both run only after the user
     clicks "Publish Engine Content" or "Rebuild Manifest from Server"
     (`EditorUI.cpp`'s `EngineContentSyncBar` only draws the footer's queue
     status, it triggers nothing itself) — no CI job clicks a button.

   So the configured endpoint line proves the credentials are compiled in, not
   that anything was fetched — there is no "assets fetched" log line to show
   from any CI run, on any platform, because no CI job exercises either code
   path above. Out of scope for this step to change (it would mean adding a
   real runtime check that opens a real network connection from CI, which is a
   product/ops decision for whoever runs the EngineContent server, not a
   build-credentials fix).
3. **A secondary, harmless source of "not configured" lines**: on a push, each
   matrix job's "Build the two app runtimes" step calls
   `scripts/build_runtimes.py`, which runs `cmake -B <tree>` again for the
   `app-advanced` and `app-basic` flavours — fresh build trees, and the
   step has no `env:` block of its own (a step's `env:` does not carry over to
   a later step). Confirmed in the same green `main` run: Linux, macOS and
   Windows each log the real endpoint once (the main `Configure` step) and then
   `... not configured` twice more (the two flavour configures). Harmless,
   because `HorizonGame` (what every flavour actually builds) never links
   `HorizonContentSync` — see point 4 — but worth naming here so the repeated
   "not configured" lines inside an otherwise-credentialed job don't look like
   a bug to the next person reading the log.
4. **Runtime flavours** (`runtime-flavors.yml`) needs no credentials at all:
   `build_runtimes.py` runs `cmake --build --target HorizonGame` specifically,
   never `all`; `HorizonGame` does not depend on `HorizonContentSync` (editor-only;
   `add_subdirectory(src/HE_ContentSync)` is still configured into the tree, but
   Ninja never builds a target nothing asked for). `claude.yml` doesn't configure
   or build the engine at all, so it is out of scope too.
5. **The self-hosted Windows path (NN-WS03) was not witnessed.** Both runs
   checked above, and the in-progress run this branch already had at the start
   of this step (`.../actions/runs/37820790573`), ran `Windows` on the hosted
   `windows-latest` runner (`gh api .../jobs/<id> --jq .runner_name/.labels`) —
   two because they were `push` events (the runner condition requires
   `github.event_name != 'push'` for self-hosted), the third because the Hive
   had `vars.WINDOWS_RUNNER` pointed back at the hosted runner while NN-WS03 was
   busy with other bees. The Windows job is the same job either way — same
   `Configure (Windows)` step, same three secrets — so there is no separate code
   path to fix, only a runner to land on while it's free; this is a "not
   witnessed yet", not an open gap.
6. **A fork pull request gets no secrets at all** (GitHub policy, not something
   this repository configures): `secrets.ENGINE_CONTENT_HOST` etc. evaluate to
   empty strings for a fork PR, so `HE_ENGINE_CONTENT_HOST`/`_USER`/`_PASSWORD`
   reach the environment as set-but-empty — the `CMakeLists.txt` block treats
   that exactly like an unset variable or a clone with no local
   `cmake/EngineContentCredentials.cmake`: host and user stay empty, the
   configure log says "not configured" (`message(STATUS ...)`, not an error),
   and the build proceeds. Not separately witnessed this step (no fork PR
   exists on this repository to point at), but it is the same CMake branch as
   the lavapipe job in `37795709487` above (point 1) — a real, witnessed,
   green "no credentials" run, not just a hypothetical.
7. **Release/packaging steps** (`Package Windows editor` / `Package macOS
   editor (DMG)` / `Package Linux editor`, and `Stage the app runtimes into the
   editor package`) are not separate `Configure` calls — they zip/tar/dmg
   whatever the one credentialed build tree above already produced, so
   whatever that job logged for the endpoint is what the uploaded editor
   artifact was built from. `Package Windows editor` is additionally gated on
   `runner.environment != 'self-hosted'` (NN-WS03 keeps its incremental
   `package/` instead of zipping it), which is orthogonal to credentials — the
   `Configure (Windows)` step upstream runs either way. The staged app runtimes
   (point 3) need no credentials, same reasoning as point 4.

Why the lavapipe job got the `env:` block added rather than a "this is
intentional" note: the job already builds `HorizonEditor` (for the image
tests), so it already links `HorizonContentSync` — leaving the block out bought
nothing, and made the one job's configure log read "not configured" next to
three jobs reading the real endpoint, which is exactly the kind of asymmetry
this step was asked to find. Per point 2, it changes nothing the image tests
actually do.


---

## Referenzen laden von selbst nach — auch per Pfad und transitiv (2026-10-09)

**Vorher:** `registerRemoteAsset` bediente nur Verweise per **UUID** (`ensureResident` / `loadAssetAsync(UUID)` — eine
Szene, die einen Default-Mesh nennt). Ein Verweis per **Pfad** — die Textur oder das Textur-Array eines Materials,
sein Parent, eine Funktion im Node-Graph, das Material eines Meshes — läuft über `loadAsset(path)`, und das fand
keine Datei und gab auf (`Cannot load asset …`); das Material blieb grau/Platzhalter, bis man die Datei von Hand
im Content Browser herunterlud. Und was ein heruntergeladenes Asset selbst referenziert, wurde erst beim ersten
Zeichnen angefragt.

**Jetzt** (`ContentManager`):

1. **Pfad → Remote-Eintrag.** `registerRemoteAsset` merkt sich den Eintrag auch unter seinem Pfad
   (`m_remoteByPath`, „Engine/Textures/…“). `loadAsset(path)` fragt vor dem Plattenzugriff
   `requestRemotePath(path)`: gibt es für den Pfad einen Remote-Eintrag **und keine lokale Datei** (echt, Override
   oder Cache — eine lokale Datei gewinnt immer), wird der Download im Hintergrund angestoßen (dedupliziert wie
   bisher: ein Renderer, der jeden Frame fragt, startet ihn einmal) und `loadAsset` meldet „noch nicht da“
   (`UUID{}`, ohne Fehlermeldung). Sobald die Datei gelandet ist, liefert die nächste Frage das Asset — mit
   der UUID, die das Manifest versprochen hat. Ein fehlgeschlagener Download lässt den Eintrag stehen; die nächste
   Anfrage versucht es erneut.
2. **Transitiv.** Jedes registrierte Asset (von Platte oder frisch heruntergeladen) stößt beim Registrieren die
   Downloads dessen an, worauf es zeigt (`prefetchRemoteReferences`): bei einem **Material** Shader, Texturen,
   **Graph-Texturen/-Arrays**, Parent-Material, **die im Graph aufgerufenen Funktionen** (Pfad und baked UUID);
   bei einem **Mesh** sein Material und das jeder Section. Die Kette setzt sich fort, weil jedes dabei ankommende
   Asset dasselbe tut. Ohne Remote-Einträge (jedes ausgelieferte Spiel, jeder Offline-Editor) kostet das einen
   `empty()`-Test.
3. **Materialien warten auf ihre Funktionen.** Ein Material mit Graph baut sein GLSL beim Laden neu aus dem Graph;
   ruft der Graph eine Funktion auf, die noch remote ist, würde das den „missing function“-Platzhalter einbacken.
   `graphFunctionsReady` lässt die Neuerzeugung dann aus (das mit dem Asset gespeicherte GLSL bleibt) und stößt den
   Download der Funktion an.
4. **Instanzen folgen ihrem Parent.** Kommt ein Material per Download an, werden die Instanzen neu abgeleitet
   (`syncMaterialInstancesOf`), die sich registriert hatten, als ihr Parent noch fehlte.

**Grenzen.** Nur Assets **mit UUID im Manifest** (rohe Dateien ohne `.hasset` gehören weiter dem Content Browser).
Nur die Verweisarten oben — Widgets (Texturen/Fonts), HorizonCode-Graphen und Sequenzen laden per UUID/Pfad
bei Bedarf, werden aber nicht vorab nachgezogen. Ein Renderer, der „Textur fehlt“ dauerhaft cachte, würde die
Ankunft verpassen; die Grafik-Backends lösen Graph-Texturen jeden Frame neu auf (`ResolveGraphTexture`) und holen
sie deshalb nach. Getestet in `test_contentmanager.cpp` („Remote EngineContent: …“, fünf Fälle mit einem
simulierten Server: Pfad-Verweis, lokale Datei nicht überschatten, Material zieht Textur, fehlgeschlagener Download,
Funktion vor Regenerierung).
