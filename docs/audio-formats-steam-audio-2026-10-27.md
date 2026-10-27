# Audio: MP3/FLAC sources and the optional Steam Audio backend

2026-10-27, branch `release/0.7.0`. What the engine's audio does with plain
miniaudio, where Steam Audio is better, and what was built to switch between them.

## 1. What miniaudio (0.11.25) gives us, and what it does not

| Area | miniaudio, as used in `AudioEngine.cpp` | Steam Audio 4.8.1 adds |
|---|---|---|
| Device / mixing | CoreAudio, WASAPI, ALSA/Pulse picked by miniaudio; one node graph; headless `noDevice` mode | nothing — it is a DSP library, not a device layer |
| Formats | WAV (PCM16 at import), Ogg Vorbis, **MP3, FLAC (new)** — compressed ones stream | nothing |
| 2D playback | volume, pitch (resampler), loop, seek, pause, trim, envelope, clip EQ, bus EQ | nothing |
| 3D position | listener + source, 3D pan, distance models (none/linear/inverse/exponential), min/max distance, rolloff | the same distance models, plus air absorption |
| Direction cue | left/right pan only (no front/back, no elevation) | **HRTF binaural**: front/back/up/down on headphones |
| Occlusion / transmission | none | ray-traced against scene geometry, per-material absorption |
| Reflections / reverb | none (no reverb node ships with miniaudio) | geometry-based reverb, baked or real time |
| Sound propagation | none | pathing around corners and through doors |
| Doppler | `ma_sound` has a Doppler factor; not wired in the engine | — |
| Ambisonics | none | encode/decode, rotation |
| Cost | trivial CPU, no extra files | HRTF per voice is cheap; reflections/pathing are the expensive part (scene BVH, simulation thread) |
| Shipping | already in the engine | ~180 MB SDK to build, one library (`libphonon`, a few MB to tens of MB per platform) to ship |

What you get **with miniaudio only** from here without Steam Audio: a reverb bus
and a compressor/limiter as custom nodes (same shape as `busEqProcess`), Doppler
(already a miniaudio parameter), a low-pass for distance/"behind a wall" faked from
a raycast done in the game, and ducking. **Steam Audio is clearly better** for
headphone localisation (HRTF), and for occlusion/reflections/pathing that follow
real geometry. For a stereo-speaker game with simple levels miniaudio is enough.

## 2. What was built

* **MP3 and FLAC** sources. `AudioEncoding::{Mp3,Flac}`, chunks `MP3D`/`FLCD`
  (a new chunk, no file-version bump: an older engine finds no data chunk and skips
  the clip). Stored as the source bytes, streamed by miniaudio's built-in dr_mp3 /
  dr_flac (`MA_NO_MP3/FLAC` removed). The importer validates with a full decode
  (vendored `dr_mp3.h`, `dr_flac.h` in `src/HE_Tools/vendor`) and keeps the bytes.
  `.mp3`/`.flac` are in `AudioImporter::isSupportedSource` and the import dialog.
* **Steam Audio backend, build flag + run-time switch**
  * `HE_ENABLE_STEAM_AUDIO` (root `CMakeLists.txt`, default ON): downloads the SDK
    at **configure** time on Windows x64 / Linux x86_64 / macOS (pinned SHA-256,
    only headers + that platform's library extracted into `<build>/_deps`);
    `-DHE_STEAM_AUDIO_ROOT=<unpacked SDK>` skips the download; a failed download or
    other platform is a warning, never a configure error. OFF, or an app runtime
    flavour: no phonon code, nothing downloaded, nothing shipped. Game trees choose
    per tree: `-DHE_ENABLE_STEAM_AUDIO=OFF`.
  * `libphonon` is opened at **run time** (`dlopen`/`LoadLibrary`, beside the engine
    library, then cwd, then the SDK folder), so there is no link step or rpath and a
    missing library means "stay on miniaudio".
  * `HorizonScene/SteamAudio.h|cpp`: `SteamAudio` (context + default HRTF) and
    `SteamAudio::Binaural` (one voice, mono in, stereo out, whole 256-frame blocks);
    `listenerRelativeDirection()` (pure math). Header is identical with/without SDK.
  * `AudioEngine::setSpatialBackend / spatialBackend / requestedSpatialBackend /
    spatialBackendStatus / steamAudioCompiledIn`. Project setting
    `AudioBusConfig::spatialBackend` (JSON `"spatial": "steamaudio"`, absent =
    miniaudio), applied by `applyBusConfig`. Audio Mixer: checkbox "Steam Audio
    (HRTF)", greyed out in a build without the SDK.
* Tests: `tests/test_audio_formats.cpp` (FLAC fixture `tests/fixtures/tone_flac.h`).

## Handoff

**Verified**
* Configure block (`CMakeLists.txt`, "Steam Audio" section): run in isolation with
  `cmake -P` against the real 4.8.1 zip: hash check, extract of only `include/` +
  `lib/osx/`, `HE_STEAM_AUDIO_ROOT` override, OFF. Real configure of `cmake-build-release`
  printed "Steam Audio enabled (…/libphonon.dylib)". Windows/Linux branches only
  checked against the zip's listing (`lib/windows-x64/phonon.dll`, `lib/linux-x64/libphonon.so`).
* `SteamAudio.cpp` (with and without `HE_HAVE_STEAM_AUDIO`) and `AudioEngine.cpp`
  pass `-fsyntax-only` with the repo's compile flags. `editor_help_audit.py --check` is green.

**Also verified (macOS arm64, Release, `DEVELOPER_DIR=/Library/Developer/CommandLineTools`)**
* `he_tests` links; `test_audio_formats.cpp` 13/13 pass, including the real
  `libphonon` load (copied next to `libHorizonScene` by the POST_BUILD step) and the HRTF
  test (hard right / hard left lean the right way, dead ahead is near the middle). Also
  green: test_audio, test_audio_edit, test_audio_bus_eq, test_audio_mixer_ui,
  test_weather_audio, test_contentmanager, test_assimpimport.
* `AudioMixerPanel.cpp` and `AudioEditorPanel.cpp` pass `-fsyntax-only`.

**Not verified**
* The editor was not built or started: the Audio Mixer checkbox was never seen on screen.
* The MP3 test uses hand-built silent frames (a pip install of an encoder was blocked), so
  it proves detection/format/length/streaming, not a signal.
* Windows and Linux builds; the `-DHE_ENABLE_STEAM_AUDIO=OFF` configuration (only the stub
  source was syntax-checked); release packaging of `libphonon` (`package_macos.sh`, exporter).

**Decided**: dlopen instead of linking; the project stores the *request* (kept when the
build lacks the SDK); default ON for editor trees, OFF/omitted by flavour for apps.
**Only suggested**: a game tree that does not need HRTF sets `HE_ENABLE_STEAM_AUDIO=OFF`.

**Next steps, in order**
1. **DONE (voice routing)**: `BinauralNode` in `AudioEngine.cpp` (sound, created with
   `NO_SPATIALIZATION|NO_DEFAULT_ATTACHMENT`, → node → bus group/endpoint). Mono downmix,
   own distance curves (`spatialDistanceGain`, miniaudio's formulas), one 256-frame block
   of latency, gain ramped per block. Listener pose copy in `Impl::listener`, source pos
   and falloff via atomics. Only when the mixer is stereo; otherwise/on failure the voice
   uses miniaudio. A voice keeps its backend for its lifetime: switching to miniaudio
   keeps libphonon open while such voices live. Test: "…rendered through the HRTF"
   (ear lean, falloff, moving source, switch-off with live voice) — 20 assertions green.
   Not verified by ear, and the stage is not exercised with bus EQ in front of it.
2. Occlusion/reflections: mirror scene geometry into an `IPLScene`
   (`iplSimulatorCreate`, needs the simulator thread) and feed direct-effect params.
3. Windows/Linux: copy `libphonon` in `package_macos.sh`-style packaging scripts
   (macOS bundle: the dylib needs to sit beside `libHorizonEngine`; its install name is `@rpath/…`).
4. Pip-free MP3 fixture with real signal, if an encoder becomes available.
