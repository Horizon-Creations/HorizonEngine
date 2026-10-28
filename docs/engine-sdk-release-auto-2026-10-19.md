# The editor build builds the release lib set too (topic 222, step 5)

Step 4 left `EngineSDK/lib/release` to CI (push-only) and to a hand-run
`scripts/build_engine_sdk.py`. A local editor build had `lib/dev` only, so a Shipping export from a
dev box was refused until somebody remembered the script. Now a dev editor tree builds the second
tree itself.

## How

`cmake/HorizonSDK.cmake`, end of file:

| Target | Tree | Does |
|---|---|---|
| `HorizonSDKLibs` | every tree | the SDK's libraries without the install (`HorizonSDK` depends on it) |
| `HorizonSDKReleaseLibs` | dev editor tree | `build_engine_sdk.py --stage build`: configure `<tree>/sdk-release` if needed, build its `HorizonSDKLibs` (console pool, live progress) |
| `HorizonSDKRelease` | dev editor tree | `--stage install`, after `HorizonSDK` (dev) and `HorizonSDKReleaseLibs`: the second tree's `HorizonSDK` + the stamp check of both sets |

`HorizonEditor` depends on `HorizonSDKRelease`, so `--target HorizonEditor` and the `dmg` target
(package_macos.sh) carry both sets. Two stages so the release libraries compile beside the dev
tree's jobs, and the two installs (shared `include/` + header stamp) still never overlap.

`scripts/build_engine_sdk.py` got `--stage all|build|install`, `--config`, `--cmake`; it configures
only when the command differs from `<build dir>/he_sdk_configure.txt` (afterwards the tree's own
ninja re-runs CMake on a changed CMakeLists), mirrors the editor tree's generator, compilers and
compiler launchers (ccache), skips empty cache values, and passes `HE_SDK_RELEASE_SET=OFF` down.

Switches (cache): `HE_SDK_RELEASE_SET` (default ON, **OFF when `$CI` is set**, so the PR runs keep
skipping the second engine build and ci.yml's push-only lane stays the CI path, unchanged),
`HE_SDK_RELEASE_JOBS` (half the logical cores, min 2), `HE_SDK_RELEASE_DIR` (`<tree>/sdk-release`;
shorten it on Windows if a deep worktree path hits MAX_PATH). Only in a tree with
`HE_BUILD_PROFILE=dev` and a `HorizonEditor`; no Python interpreter -> a configure warning.

## Measured (M5 Air, on power, shared ccache, `out/build/release`, `-j8`)

| | `HE_SDK_RELEASE_SET=OFF` | `ON` |
|---|---|---|
| no-op `--target HorizonEditor` (2 runs) | 0.57 / 0.58 s | 1.53 / 1.74 s |
| one engine .cpp touched (`Log.cpp`, ccache hit, 2 runs) | 15.4 / 14.4 s | 14.0 / 13.7 s |
| first build after switching it on (lib/release removed before) | — | 531 s: nested configure 67 s, then the release libs at -j5 (step 4's objects sat under another build path) |

The touched .cpp costs nothing measurable: both trees relink in parallel and the editor link is the
longer pole. A real source edit compiles that TU twice (once per tree, in parallel). Not measured:
a header edit (we do not touch headers to measure); its upper bound is the first build's compile
part, since the release tree has only the engine libraries (no editor, no tests).

Verdict: the extra cost is about 1 s per build plus a one-time first build, so the release set is
built always. The middle ground the step suggested (rebuild only on header change) was not built:
it would ship a release library with stale engine code after a .cpp edit, and the stamp check would
not notice.

## Handoff

- **Topic / step:** 222, step 5. Branch `claude/engine-sdk-wird-nicht-mitgebaut-gepackaged-horizonsdk-target-v2`.
- **Done:** `cmake/HorizonSDK.cmake` (end: `HorizonSDKLibs`, `HE_SDK_RELEASE_SET`, two targets,
  editor dependency), `scripts/build_engine_sdk.py` (stages, configure skip, mirrored toolchain),
  `docs/engine-sdk-release-lane-2026-10-19.md` (local note superseded).
- **Verified (Mac):** with `lib/release` deleted, one `--target HorizonEditor` rebuilt it (rc 0), `--verify
  --require dev --require release` ok, same stamp; timings above; `scripts/he_sdk_export_live.py`:
  `Development=True Shipping=True no_compiler=True`, Shipping log "Game compiled from the SDK
  (release)"; `build_profile_strings.py --profile release` OK on the Shipping export and on lib/release.
  `--target dmg` (package_macos.sh, 31 s on top): "engine SDK (lib sets: dev release) -> Resources/EngineSDK",
  `--verify` on `out/dmg_staging/HorizonEditor.app/Contents/Resources/EngineSDK` ok for both.
  he_tests (all but test_material_graph): 5492/5495, the three known ones (test_hpak.cpp:1188 user
  content cache, test_profiler.cpp:107/215 order) pass alone / with an empty HOME.
- **Not verified:** Windows/Linux local builds with the option ON (CI has it OFF); a header edit's cost;
  a CLion build without ccache in the environment (the launchers are mirrored from the cache).
- **Decisions:** always ON locally, OFF in CI by `$CI`; nested tree inside the editor tree (deleting the
  tree deletes both); the script stays the one place that knows how to make the second tree.
- **Traps:** every dev tree's first `--target HorizonEditor` now also builds `<tree>/sdk-release`
  (~8 min on the Air; ccache mostly misses across worktrees, the build-relative paths differ);
  `--target he_tests` does not. A compile error only the release profile has (code around
  `#if HE_FEATURE_*`) now breaks the editor build; the cause is in the nested tree's console output.
- **Next steps:** 1. Optionally switch CI's push lane to the editor-built set (`-DHE_SDK_RELEASE_SET=ON`
  on push) and drop the separate step; needs a CI run per platform. 2. Windows dev box check of the
  nested tree path length.
