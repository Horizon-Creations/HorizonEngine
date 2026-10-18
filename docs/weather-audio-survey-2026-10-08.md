# Wetter 2.0: weather audio and EngineContent credentials, survey (step 1)

Topic 167, step 1, 2026-10-08. Read-only survey: no code was changed. The Hive MCP
server did not connect in this session (`CONNECT_TIMEOUT`), so this file is the
report and the handoff (last section). Everything below was read from the tree at
`release/0.7.0` (`7abbfc96`) unless a CI run is named.

## 1. Weather code

Files: `src/HE_Scene/include/HorizonScene/WeatherSystem.h`,
`src/HE_Scene/src/WeatherSystem.cpp` (316 lines),
`src/HE_Scene/include/HorizonScene/Components/WeatherComponent.h`,
`tests/test_weather.cpp` (31 test cases).

### Weather types

`enum class WeatherKind { Clear, Cloudy, Overcast, Foggy, Rain, Storm, Snow, Count }`.
`PrecipType { None, Rain, Snow }`. The preset table is `weatherPreset()`
(`WeatherSystem.cpp:13-27`):

| Kind | cloud | fog | wind | precip | type | wetness | lightning |
|---|---|---|---|---|---|---|---|
| Clear | 0.05 | 0 | 0.8 | 0 | None | 0 | no |
| Cloudy | 0.45 | 0 | 1.2 | 0 | None | 0 | no |
| Overcast | 0.85 | 0.01 | 1.4 | 0 | None | 0.1 | no |
| Foggy | 0.60 | 0.08 | 0.4 | 0 | None | 0.2 | no |
| Rain | 0.90 | 0.03 | 1.6 | 0.7 | Rain | 0.8 | no |
| Storm | 1.00 | 0.04 | 2.6 | 1.0 | Rain | 1.0 | yes |
| Snow | 0.80 | 0.05 | 0.9 | 0.6 | Snow | 0.3 | no |

### Intensity and transitions

- Authored on `WeatherComponent`: `currentKind`, `targetKind`, `intensity` (0..1),
  `transitionDuration` (8 s), `autoCycle` + `cycleSeconds`, `thunderSound` (UUID),
  `maxRainParticles`/`maxSnowParticles`, `groundLevel`.
- `WeatherSystem::update(world, dt, cameraPos, physics, gpuPrecip)` takes the first
  `WeatherComponent` in the registry (`WeatherSystem.cpp:83-86`). The component lives
  on an ordinary "Weather" entity, found with `HorizonWorld::weatherEntity()`. The
  old note that it sits on the World root is outdated.
- Target values are the preset values scaled by `intensity` (wind is
  `1 + (preset - 1) * intensity`, so intensity 0 means calm, not zero).
- A change of `targetKind` snapshots the live `EnvironmentComponent` values as the
  origin, then lerps with `smoothstep(transitionElapsed / transitionDuration)`
  (`:125-154`). At `a >= 1` `currentKind = targetKind`.
- Values are written into `EnvironmentComponent` (cloud, fog, wind, `rainAmount`,
  `snowAmount`, `wetness`). A field is only driven while the live value equals what
  weather wrote last (`lastCloud` ...), so a slider the user moves is respected until
  a new preset is picked (`reclaim`).
- `curPrecip = max(rainAmount, snowAmount)` and `curPrecipType` are the display
  mirror (`:180-187`). **These are the natural inputs for a rain/snow loop volume.**
  `curWindSpeed` is the input for wind.
- Lightning (`:189-207`): only when the TARGET preset has `lightning` and
  `intensity > 0.05`. Countdown `U(2.5, 11) * (1.2 - intensity)` s; a strike sets
  `flashIntensity = 1`, `flashTriggered = true` for exactly that frame, decays at 6/s,
  and writes `env->flash`.
- Particles are a CPU pool (`precip`) unless `gpuPrecip` (then skipped, but the env
  values are still written).

### Where weather ticks

`SceneSystems::tickWorld` (`SceneSystems.cpp:131`) calls `WeatherSystem::update`. Both
apps call it: the editor every frame, edit mode included (`EditorApplication.cpp:3383`),
and `GameApplication.cpp:3298`.

## 2. Audio layer

- `AudioEngine` (`src/HE_Scene/include/HorizonScene/AudioEngine.h`, miniaudio):
  `play(AudioAsset, volume, pitch, loop, busName)`, `playSpatial(...)`, handle-based
  live control (`setSoundVolume`, `setSoundPitch`, `setSoundLooping`, `stop`,
  `isPlaying`), buses (`createBus`, `setBusVolume`, `setBusMuted`, `routeFor`),
  master volume, `init(noDevice=true)` for tests, `readMixedFrames` to pull the mix
  headless. PCM16 and Ogg Vorbis assets; PCM16 copies the samples into the voice.
- Mixer groups = buses. `HE::AudioBusConfig` (`HE_Core/include/Audio/AudioBusConfig.h`)
  is a PROJECT setting (`.heproj` -> `project.hcfg`) and is **empty by default**; the
  mixer window offers Music/SFX/Voice with one click. A voice asking for a missing bus
  falls back to master with a warning (`routeFor`). So weather audio cannot assume a
  "Weather" bus exists.
- `AudioSourceComponent` / `AudioListenerComponent` / `AudioSystem`
  (`HorizonScene/AudioSystem.h`): `playOnStart` once on entering play,
  `updateSpatial` per frame. Neither starts anything in edit mode.
- Clip edits (trim, volume curve, bus, EQ) live in the asset (`AudioAsset::edit`).
- Lesson already on the board: `getAudio()` pointers die at the next `loadAsset`, and
  `AudioEngine::play` copies the data for that reason.

### The gap this topic has to close

1. `thunderSound` is consumed in exactly one place: `EditorApplication.cpp:3415-3426`,
   inside `m_isPlaying`, via `m_audioEngine.play(*a)` (no bus, no volume, no spatial).
   **`GameApplication` has no equivalent, so a shipped game is silent on thunder
   today.**
2. There are no rain, wind or snow sounds and no `WeatherComponent` field for them.
3. `SceneSystems::tickWorld` has no `AudioEngine` parameter. The two apps feed audio
   separately (`AudioSystem::updateSpatial`: `EditorApplication.cpp:3412`,
   `GameApplication.cpp:3215`), so weather audio needs one shared HE_Scene function
   called next to those, not code inside `WeatherSystem::update`.
4. "Hearable in the editor": the editor engine is initialised (`init()` at
   `EditorApplication.cpp:1744`), but every weather audio hook is play-mode only.
   The edit-mode viewport needs its own (switchable) path.

Places that must follow when `WeatherComponent` gets sound fields:
`SceneSerializer.cpp:539` (write) and `:1397` (read, JSON; check the CBOR path),
`SceneSystems.cpp:284` (`collectAssetRefs`, otherwise the sound is not packed),
`InspectorPanel.cpp:1331` (the Thunder drop slot), `tests/test_scene_serializer.cpp:1887/2258`.

## 3. EngineContent and standard assets

- Source tree: `EditorDeps/EngineContent` (4.0 MB: MaterialFunctions, Materials,
  Meshes, Particles, Textures/Landscape, Widgets). **No Audio folder exists, and no
  `.wav`/`.ogg`/`.mp3`/`.flac` is tracked anywhere in the repo.**
- All of it is produced by deterministic one-shot generators in
  `src/HE_Tools/src/{MeshGen,WidgetGen,MatFnGen,MatGen,LandscapeTexGen}` (targets
  `mesh_gen`, `widget_gen`, ..., written through `ContentManager::saveAsset`, outputs
  committed, "not part of the normal build graph"). Every asset gets a well-known
  UUID `{hi = block + index, lo = 1}`: 0x100 mesh, 0x200 widget, 0x300 matfn,
  0x400 mat/landscape textures (up to 0x411), 0x412 auto landscape material.
  **0x500 and up is unused.**
- **Pre-existing finding, not fixed here:** `Materials/Water.hasset` and
  `Textures/Landscape/T_Landscape_Grass_Albedo.hasset` both carry UUID
  `{0x400, 1}` in their META chunk (`MatGen` kMatBaseHi = 0x400 with Water at index 0,
  `LandscapeTexGen` kTexBaseHi = 0x400 with Grass_Albedo at index 0; verified by
  reading the META bytes of both files). Two shipped engine assets share one UUID.
  Not part of this topic; the queen should decide. It also means: pick a fresh block
  for the weather sounds.
- Resolution of an `"Engine/..."` path (`ContentManager::resolveAbsolutePath`):
  project override (`Content/Engine/...`), then the shipped default
  (`engineContentRoot()` = `EditorDeps/EngineContent` next to the executable), then
  `GlobalState::engineContentCacheDir()` (the SFTP cache). The editor scans these
  roots into the UUID registry, so a `.hasset` with a fixed UUID is found by UUID.
- The packaged game has **no** `engineContentRoot` (`ContentManager.h:527-531`). The
  exporter packs engine assets into the `.hpak` only if a scene references them
  (`ProjectExporter::engineContentDir`, `SceneSystems::collectAssetRefs`). This is
  why every new UUID field on `WeatherComponent` must be added to `collectAssetRefs`.
- SFTP sync (`src/HE_ContentSync`, design in `docs/engine-content-sftp-sync-design.md`):
  editor-only, libssh2, `manifest.json` of `{path, uuid, hash, size}`; remote-only
  assets are registered with `ContentManager::registerRemoteAsset` and materialised on
  demand (`ensureResident` never blocks on the network, it starts the download and
  returns false). Files already in git are always local; the SFTP library is an
  additional source, filled by the dev-side "Publish Engine Content" action. Publishing
  sends files to an external server, so that is a human decision, not something an
  agent run does.
- Loading rule for sounds at runtime: the asset must be resident before
  `getAudio()` returns it (scene load preloads whatever `collectAssetRefs` lists).
- No Ogg encoder is in the tree (`AudioImporter` reads `.wav` and `.ogg`, stb_vorbis
  is decode-only). Generated sounds therefore go in as PCM16. Size guide: 8 s mono at
  22.05 kHz is about 350 KB per loop; five loops plus a few thunder one-shots come to
  roughly 2 to 3 MB, in the range of the existing Landscape textures (2.3 MB).
- Licence: nothing third-party exists, so the only clean route is to synthesise the
  sounds (filtered noise for rain/wind, shaped noise bursts for thunder, circular
  synthesis or baked cross-fade so loops are seamless) in a new generator in the
  `*_gen` pattern. Self-generated, no licence question.

## 4. CI credentials (input for step 4, not the verdict)

Workflow `.github/workflows/ci.yml`:

| Job | Configure step | `HE_ENGINE_CONTENT_HOST/USER/PASSWORD` from secrets | Evidence |
|---|---|---|---|
| macOS (matrix) | `:236-242` | yes | log: `HorizonContentSync: EngineContent endpoint ***@***:22` |
| Linux (matrix) | `:247-253` | yes | same line |
| Windows (matrix) | `:258-264` | yes | same line |
| Linux Vulkan (lavapipe) | `:600-601` | **no `env:` block** | log: `EngineContent endpoint not configured` |

Evidence is run 37795709487 (push to `release/0.7.0`, hosted runners), read with
`gh api repos/Horizon-Creations/HorizonEngine/actions/jobs/<id>/logs`. In the three
matrix jobs there was no "password is empty" warning. `***@***` is GitHub's masking of
the secret values, which also proves host and user are both non-empty (otherwise
CMake prints "not configured").

Notes for step 4:
- The credentials only reach the build as compile definitions of `HorizonContentSync`
  (`src/HE_ContentSync/CMakeLists.txt`): environment variables, never `-D`, so they
  stay out of `CMakeCache.txt`. `BASEPATH` is not forwarded by CI (defaults to the
  account root); `PORT` is deliberately not environment-driven.
- The lavapipe job builds `HorizonEditor` for image tests and uploads only screenshots.
  Its editor is not packaged, so "not configured" is harmless there. Decision for step
  4: either add the same `env:` block for consistency or report it as intentional.
- Not witnessed: (a) the self-hosted NN-WS03 Windows path (topic branches, PRs,
  dispatches), which uses the same step and secrets; (b) `pull_request` runs from
  forks, where GitHub withholds secrets by policy (the runner condition already keeps
  those off NN-WS03). A run on this topic branch (`gh workflow run CI --ref <branch>`)
  would show (a).
- `.github/workflows/runtime-flavors.yml` and `scripts/build_runtimes.py` configure the
  game runtime flavours; `HE_ContentSync` is editor-only and `HE_Game` does not link
  it, so credentials are not needed there.

## 5. Traps for the next steps

- `tests/` only copies a freshly built `HorizonScene.dll` next to `he_tests` when
  `he_tests` itself relinks (kontext lesson): after a change confined to
  `AudioEngine.cpp` or a new HE_Scene source, copy `build/src/HE_*/Horizon*.dll` into
  `build/tests/` or compare hashes before believing a result.
- New code in `HorizonScene`/`HorizonGame` can trip the Windows `runtime_size` test
  (`scripts/runtime_size.py` LIMITS["win32"]["game"]); a green run prints no size
  report. Measure with `python3 scripts/runtime_size.py` on `out/deploy/Game`.
- `m_audioEngine.play` for a looping voice returns a handle; weather audio has to own
  the handle, apply volume with `setSoundVolume`, and `stop` on scene change, play
  stop and weather removal, or voices leak across projects.
- Weather freezes with the editor pause and `timeScale`; the audio must follow, not
  keep a rain loop running on a paused viewport.

## Handoff

- **Done:** survey of weather code, audio layer, EngineContent/ContentSync and the CI
  credential flow, with CI log evidence for all four CI jobs. No code changed.
- **Pending:** steps 2 to 5 of the topic (weather audio, standard assets, CI
  verdict, verification).
- **Decisions suggested, not taken:**
  - One shared HE_Scene function (for example `WeatherAudio::update(world, engine,
    content, dt, ...)`) called next to `AudioSystem::updateSpatial` in both apps; it
    replaces the editor-only thunder block. The sound set is per `WeatherKind`
    (rain, wind, snow/storm bed, thunder) and cross-faded from `curPrecip`,
    `curPrecipType`, `curWindSpeed` and the blend, so gameplay code does nothing.
  - Sounds as PCM16 `.hasset` files under `EditorDeps/EngineContent/Audio/Weather/`,
    committed like the other generated content, made by a new `*_gen` tool with UUID
    block 0x500.
  - Make weather audio audible in the edit-mode viewport, with an off switch.
- **Fallen / not done:** Hive tools (`hive_join`, `hive_status`, `hive_claim`,
  `hive_post`, `hive_step_done`, `hive_handoff`, `hive_leave`) were never available,
  three ToolSearch attempts returned "hive: CONNECT_TIMEOUT". Nothing was claimed or
  posted on the board. The UUID collision above was found, not fixed.
- **Next steps:** read this file, then step 2. Open question for the queen: UUID
  collision of Water and Grass_Albedo at `{0x400, 1}`.
