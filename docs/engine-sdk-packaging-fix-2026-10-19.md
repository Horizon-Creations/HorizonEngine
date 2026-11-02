# Engine SDK built with the editor and packaged: fix (topic 222, step 2)

Step 1 (`docs/engine-sdk-packaging-befund-2026-10-19.md`) found that the `HorizonSDK` target
hung off nothing. This step implements its items 1 and 2: the editor build installs the SDK, and
the packages carry it. Item 3 (a release lib set from a release-profile tree in CI) is step 3.

## What changed (commit f154bac4)

| Where | What |
|---|---|
| `cmake/HorizonSDK.cmake` (end) | `add_dependencies(HorizonEditor HorizonSDK)` inside `if(TARGET HorizonEditor)`. It sits here and not beside the HorizonGame dependency in `CMakeLists.txt:976`, because the target only exists after this file is included (a `TARGET HorizonSDK` guard there would be silently false). Ninja makes it order-only on the editor link (`\|\| HorizonSDK`), so the editor does not relink because the SDK was copied. The "Not ALL" comment now explains that trees without an editor build it on request only. |
| `scripts/package_macos.sh` (after the `SDK/` block) | Copies `$SOURCE_DIR/out/deploy/EngineSDK` to `Resources/EngineSDK` before signing. If the folder is missing, it warns and goes on (rc 0). `dmg` depends on `HorizonEditor` (`src/HE_Editor/CMakeLists.txt:730`), so `--target dmg` finds the SDK fresh. |
| `.github/workflows/ci.yml` ("Stage the engine SDK into the editor package") | Windows/Linux: `mv package/EngineSDK package/Editor/EngineSDK` on every run (not push-only like the app runtimes: every editor build installs the SDK). A missing folder fails the step with `::error`. `DEPLOY_ROOT` falls back to `package`, because the push-only runtime step is the one that exports it. |

## Before / after (MacBook Air M5, Release, `HE_BUILD_PROFILE=dev`, shared ccache)

| | before (9086b900) | after (f154bac4) |
|---|---|---|
| `out/deploy/EngineSDK` after `--target HorizonEditor` | absent | present, 39 MB (`include/`, `lib/dev`, `lib/cmake`, `share/`) |
| no-op `--target HorizonEditor` (3 runs) | 0.22 / 0.22 / 0.25 s, "no work to do" | 0.98 / 0.99 / 1.01 s, 2 steps (glob check + SDK install) |
| one engine .cpp changed (`HE_Scene/.../ValueWire.cpp`, 2 runs) | 14.8 / 14.1 s, 9 steps | 13.1 / 12.7 s, 10 steps (noise is larger than the delta) |
| first build after the change (reconfigure + first install) | | 5.4 s |
| `--target dmg` with `out/deploy/EngineSDK` deleted first | | rc 0, 36 s; installs the SDK, then logs "engine SDK (lib sets: dev) → Resources/EngineSDK" |
| DMG size | 58.0 MB (same script, SDK folder absent) | 70.2 MB (+12.2 MB compressed) |
| `codesign --verify --deep --strict` staged .app | | OK ("Signed.", ad-hoc) |
| Export, Development, "Compile game from SDK" (live editor) | falls back to the prebuilt game | `Engine SDK (dev): <worktree>/out/deploy/EngineSDK`, "Game compiled from the SDK (dev), 3 file(s) replaced". The game runs 120 hidden frames, rc 0, "project configuration compiled in", no `/opt/homebrew` link |
| Export, Shipping, same | falls back | still falls back: "no engine SDK with the 'release' lib set" (step 3) |

The cost is the install alone. It has no OUTPUT, so it runs on every editor build: `lib/dev` gets wiped and
copied again, and the headers come out "Up-to-date". That is +0.8 s on a no-op build, inside step 1's
0.7-1.4 s estimate. I did not switch to a stamp-file custom command. Revisit that only if someone
complains about the no-op time.

## Verification

* Build: fresh `out/build/release` in this worktree (configure 75 s, `--target HorizonEditor` 885 s
  from scratch with ccache), before and after the change. Graph: the HorizonEditor link edge in
  `build.ninja` carries `|| HorizonSDK`.
* Export dialog: the checkbox is never disabled. The dialog prints "Will use the prebuilt game: <why>"
  from `findGameSdk(SDL_GetBasePath(), profile)` + `unavailableReason` (`ExportDialogPanel.cpp:1213-1231`).
  I checked that exact path in the real editor with MCP `project_package` (profiles Development and Shipping,
  `compileGameFromSdk: true`, `HE_GAME_SDK_DIR` unset): `python3 scripts/he_sdk_export_live.py <outdir>`
  (rc 0, "RESULT dev_compiled=True release_compiled=False"). It reuses `he_rewards_live.py`'s Editor/setup/export.
  Step 3 runs it with `--require both`. I did not look at the ImGui frame itself.
* `sdk_example_project` (ctest, label `sdk`) installs into `<build>/sdk-test/sdk-installed`, not into
  `HE_ENGINE_SDK_DIR`, so CI's ctest does not disturb `package/EngineSDK` before the staging step.
* `HE_EXPORT_SDK_E2E=out/deploy/EngineSDK he_tests --source-file='*test_export_sdk_build.cpp'`: 11/11
  cases, 136/136 assertions. It builds a game and compiled HorizonCode against the auto-installed SDK.
* Full `he_tests` (from the build dir, `HE_CONFIG_DIR` unset): 5591/5594, 522 s. None of the 3 failures
  comes from this change. This diff touches no C++ and no compile definition, so the binary is
  release/0.7.0's, and CI was green on a41f0964. The two `test_profiler.cpp` cases (107, 215) pass alone
  (18/18), so they fail only in the full run, depending on test order. `test_hpak.cpp:1188` (scan 6 ≠ 2) passes with
  `HOME=<empty dir>`: `scanContentDirectory` also scans `GlobalState::engineContentCacheDir()`, and this Mac's
  real `~/Library/Application Support/HorizonEngine/EngineContentCache` adds 4 assets.
* CI (Windows/Linux staging step): not verified locally. The YAML parses (ruby), and the step order is
  build → tests → stage runtimes → stage SDK → .heproj checks → package.

## Handoff

- **Topic / step:** 222, step 2. Commit f154bac4 (fix), this doc, `scripts/he_sdk_export_live.py` (witness).
- **Done:** HorizonEditor → HorizonSDK dependency, the .app copy, CI staging, and the measurements above.
- **Verified:** everything in the table, on this Mac, in this worktree's `out/build/release` and `out/deploy`.
- **Not verified:** Windows/Linux (CI run started at the end of the step, see the Hive post); a Shipping
  export compiled from an SDK (no release set exists); the human's real DMG release flow.
- **Decisions:** dependency in `HorizonSDK.cmake`, not in the root CMakeLists. No stamp output (the +0.8 s is
  accepted). Missing SDK: package_macos.sh warns, CI fails (a CI build always has an editor).
- **Fallen:** a `TARGET HorizonSDK` guard placed at `CMakeLists.txt:976` is always false (the target does not
  exist yet). `package_macos.sh` writes `out/HorizonEditor-<ver>.dmg` under the source dir, which is safe in a
  worktree and not in the main checkout.
- **Next steps:** 1. Step 3: a release-profile tree in CI (`HE_BUILD_PROFILE=release`, `HE_BUILD_TESTS=OFF`,
  `--target HorizonSDK`, same `DEPLOY_DIR`) before packaging, so `lib/release` lands in the same
  `EngineSDK/`. On Windows/Linux before the new staging step; on macOS before `--target dmg`. 2. Optionally
  merge `SDK/` and `EngineSDK/` (#135, step 1 doc).
