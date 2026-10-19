# The engine SDK's release lib set, in CI and locally (topic 222, step 4)

Since step 3 every export compiles its game from the engine SDK, and a Shipping export needs
`EngineSDK/lib/release`. A tree installs only the lib set of its own `HE_BUILD_PROFILE`
(`cmake/HorizonSDK.cmake`), and the editor tree is dev, so until this step no build anywhere
produced `lib/release`: Shipping exports were refused in every packaged editor.

## What builds it

`scripts/build_engine_sdk.py --editor-tree <dev tree>` configures and builds a second tree
(`HE_BUILD_PROFILE=release`, `HE_BUILD_TESTS=OFF`, `--target HorizonSDK` only). Everything but
the profile comes from the editor tree's `CMakeCache.txt`:

| Taken over | Why |
|---|---|
| `HE_ENGINE_SDK_DIR` | the set lands in the same `EngineSDK/` as `lib/dev` |
| `MIRRORED` options (`HE_PREFER_MBEDTLS`, `HE_PORTABLE_BUILD`, `HE_PROFILING`, `HE_ENABLE_SHADERC`, ...) | the release lib must link what the dev lib links (e.g. no Homebrew libcrypto next to a static-mbedTLS dev set) |
| every FetchContent source (`_deps/*-src` and the tree's own `FETCHCONTENT_SOURCE_DIR_*`) | `include/` is shared and stamped; another glm/nlohmann/SDL checkout would rewrite it and make the dev set "not found" |

Its `DEPLOY_DIR` is its own (`<build dir>/deploy`): the editor's deploy holds the dev `Game/`.
After the install it checks that every lib set in the SDK carries the stamp `include/` has now
(`HorizonEngineHeaders-<profile>.txt == HorizonEngineHeaders.txt`) and fails otherwise.
`--verify <EngineSDK> --require dev --require release` runs only that check.

## CI (`.github/workflows/ci.yml`)

* **Build the engine SDK's release lib set:** after the runtime size check, before the SDK staging
  (Win/Linux) and the DMG (macOS). `if: github.event_name == 'push' || inputs.release_sdk`: push-only
  like the app runtimes. A dispatch opts in with `gh workflow run CI --ref <branch> -f release_sdk=true`.
  Tree `build-sdk-release/`, kept by NN-WS03's tidy step.
* **Stage the engine SDK into the editor package** (Win/Linux): when the release step ran, it
  runs `--verify` on `package/Editor/EngineSDK` (both sets required).
* **Check the engine SDK lib sets inside the macOS package:** `--verify` on the staged
  `out/dmg_staging/HorizonEditor.app/Contents/Resources/EngineSDK`, after the dmg target's editor
  build has reinstalled `lib/dev` on top.

Locally nothing changes for an editor build: whoever wants Shipping exports from their own
`out/deploy` runs `scripts/build_engine_sdk.py --editor-tree out/build/release --jobs 8` once.
The editor build afterwards replaces `lib/dev` only (`HorizonSDK.cmake` removes `lib/<profile>`).

## Measured (M5 Air, battery, shared ccache)

| | |
|---|---|
| configure, first try (sources only from the tree's `_deps`) | 222 s, cloned glm/SDL/... again, because this worktree's tree borrows them from the main checkout |
| configure, with the tree's `FETCHCONTENT_SOURCE_DIR_*` taken over | 62 s, no clone |
| build `--target HorizonSDK -j8`, 736 steps | 205 s (warm ccache, a release tree was built in topic 211; CI's first run per branch cache is cold) |
| SDK size | `lib/dev` and `lib/release` 3 files each (libHorizonEngine, libHorizonPython, libSDL3) |

## Handoff

- **Topic / step:** 222, step 4 of 4. Branch `claude/engine-sdk-wird-nicht-mitgebaut-gepackaged-horizonsdk-target-v2`.
- **Done:** the release lib set has a builder (`scripts/build_engine_sdk.py`) and a CI lane (push-only,
  dispatch opt-in), and both packages carry `lib/release` with a stamp check. Shipping exports compile.
- **Files:** `scripts/build_engine_sdk.py` (new), `.github/workflows/ci.yml` (input `release_sdk`, the build
  step, `--verify` in the staging step and after the DMG, tidy keeps `build-sdk-release/`),
  `cmake/HorizonSDK.cmake:244` (stale "fell back to the prebuilt game" comment replaced).
- **Verified (Mac, this worktree):** release tree built (rc 0), `--verify`: dev and release ok, same stamp
  `dfb5eb6d...`; negative controls: changed release stamp gives MISMATCH/rc 1, missing set gives rc 1.
  `build_profile_strings.py --profile release` on `lib/release`: OK (dev set: 31 items still in).
  `otool -L` of both libHorizonEngine identical. Reverse order: `--target HorizonEditor` on the dev tree
  (reconfigured by the comment edit, reinstalled lib/dev): `lib/release` still there, stamps equal.
  `scripts/he_sdk_export_live.py /tmp/sdk4_live`: `Development=True Shipping=True no_compiler=True`;
  Shipping log "Game compiled from the SDK (release)"; `build_profile_strings --profile release` on the
  Shipping export folder OK, on the Development one 33 items present.
- **Not verified:** the CI lane itself on Windows/Linux/macOS (dispatch with `release_sdk=true` started at
  the end, see the Hive post); CI time per platform (the 5-9 min is an estimate from the app flavours);
  a release game started (no hidden mode in release); `package_macos.sh` run locally with both sets.
- **Decisions taken:** own script, not a `build_runtimes.py` flavour (that one deploys runtimes and checks
  `<deploy>/<Dir>`; this installs into the SDK and checks stamps); it imports `find_generator` from there.
  Options are read from the editor tree instead of being repeated in ci.yml. Push-only + dispatch input.
  The editor's "no '<profile>' lib set" text is unchanged.
- **Suggested, not taken:** a ctest that runs `--verify` on `out/deploy/EngineSDK` when both sets exist;
  the export dialog's missing-set line could name `scripts/build_engine_sdk.py` for dev boxes.
- **Traps:** a worktree's tree has no `_deps/*-src`, its sources are in `FETCHCONTENT_SOURCE_DIR_*`
  (first configure cloned everything again). The release tree must never share the editor's
  `DEPLOY_DIR`. `--define` overrides the mirrored options (comes later on the command line).
- **Next steps:** 1. Read the dispatch run's three "Build the engine SDK's release lib set" steps for time
  and the `--verify` lines. 2. After the merge into release/0.7.0 the push run builds it for the published
  packages; the first one is cold in the compiler cache.
