# Engine SDK is neither built nor packaged: finding (topic 222, step 1)

Topic 211 added the `HorizonSDK` target (cmake/HorizonSDK.cmake). Nothing builds it. A normal
editor build, local or packaged, has no `EngineSDK/`, so "Compile game from SDK" in the export
dialog always falls back to the prebuilt runtime.

**Branch base first:** this branch is cut from `main` (1c1fd193). `main` has none of the
topic 211 files (`cmake/HorizonSDK.cmake`, `HorizonEngineBundle.cmake`, `cmake/sdk/`,
`src/HE_Editor/ExportSdkBuild.*`). They exist only on `release/0.7.0`. Every path and line
below refers to `origin/release/0.7.0`. Step 2 cannot edit them on this branch as it stands.

## Cause (release/0.7.0)

| Where | What |
|---|---|
| `cmake/HorizonSDK.cmake:235-243` | `add_custom_target(HorizonSDK ...)` is deliberately not `ALL` ("an engine build should not pay for a copy nobody asked for"). It runs `cmake --install <tree> --component HorizonSDK --prefix ${HE_ENGINE_SDK_DIR}`, default `${DEPLOY_DIR}/EngineSDK`. |
| `CMakeLists.txt:972-981` | `add_dependencies(HorizonEditor HorizonGame)` exists, and for the same reason (a missing `out/deploy/Game` gave dead exports). There is no `add_dependencies(HorizonEditor HorizonSDK)`. The file is only `include()`d. |
| `cmake/HorizonSDK.cmake:79`, `:94-98` | Installs `lib/${HE_BUILD_PROFILE}` only, so a tree produces **one** lib set. The editor tree is `dev`. A `release` set needs a separate `HE_BUILD_PROFILE=release` tree (editor and he_tests do not compile there). |
| `src/HE_Editor/ExportSdkBuild.cpp:56-71` | `findGameSdk` looks in `HE_GAME_SDK_DIR`, then `<editorBase>/EngineSDK` (package; for a .app `<editorBase>` = `Contents/Resources`), then `<editorBase>/../EngineSDK` (= `DEPLOY_DIR/EngineSDK` in a dev tree). Shipping wants `release` (`sdkProfileFor(false)`), Development wants `dev`. |
| `scripts/package_macos.sh:232-263` | Copies `Game`, `AppAdvanced`, `AppBasic`, `cmake` and `SDK` (the HorizonCode codegen SDK). It never copies `EngineSDK`. |
| `.github/workflows/ci.yml` | No `--target HorizonSDK`, no `HE_BUILD_PROFILE=release` tree (nor in `scripts/build_runtimes.py`). Windows/Linux ship `package/Editor` as a whole. `HE_ENGINE_SDK_DIR` would land in `package/EngineSDK`, *beside* Editor/, so it would also need the same move into Editor/ that the app runtimes get ("Stage the app runtimes into the editor package"). |

Topic 211 itself left this open: handoff step 5 "Decide how the editor package carries a
release SDK (second tree in CI packaging)", and step 9 next steps item 3 "Ship `EngineSDK` (both
sets) in the editor package (CI)". All of topic 211's verification used a hand-built SDK
(`--target HorizonSDK` in `out/build/release` and in `out/build/release-profile`).

## Reproduced on macOS (19.10.)

* `cmake-build-release` of the main checkout (`release/0.7.0`, `HE_BUILD_PROFILE=dev`, editor
  built today 10:00): `out/deploy/` holds `Editor/` and `Game/` but **no `EngineSDK/`**.
  `HE_ENGINE_SDK_DIR` points there. In `build.ninja`, HorizonSDK appears only on its own
  edges (lines 228, 336). `build all:` has 0 SDK entries. The HorizonEditor edge (17446, phony
  24972) does not reach it. `install_manifest_HorizonSDK.txt` was absent, so that tree had never
  installed the SDK.
* Packager: release/0.7.0's `package_macos.sh` run from an isolated source dir
  (`/tmp/he222pkg`: script + CMakeLists + dmg_assets, `out/deploy/Editor` and `EditorDeps` as
  symlinks, so the human's `out/*.dmg` stayed untouched): rc 0, 70 s. The staged `.app`
  has `Resources/{Docs,EngineContent,Fonts,Game,Images,SDK,cmake,...}`, no
  `Resources/EngineSDK` and no `Contents/EngineSDK`. The editor binary is SDK-aware (3 "EngineSDK"
  strings). The released `out/HorizonEditor-0.7.0.dmg` (15.10.) has no EngineSDK either, but it
  predates the SDK export, so it shows nothing for this bug.
* Cost of the missing step: `cmake --install cmake-build-release --component HorizonSDK
  --prefix /tmp/...` took 1.42 s cold and 0.65 s warm. The dev set is 39 MB (include 11 MB, lib/dev 27 MB).
  The lib set is wiped and copied again every time (`HorizonSDK.cmake:92`), and the target has no
  OUTPUT, so it runs on every build.
* Not done: a fresh from-scratch configure+build (17-33 min on this machine). The build
  graph above answers the same question.

## Fix approach (decision for step 2)

1. **Dev tree:** `add_dependencies(HorizonEditor HorizonSDK)` right after the HorizonGame line
   (`CMakeLists.txt:976`). HorizonGame already links `libHorizonEngine`, so the bundle is in the
   editor's closure. Expected extra cost per incremental editor build is about 0.7-1.4 s, the
   install alone. Step 2 measures it. This gives the **dev** set only, so Development exports work.
2. **Package:** `package_macos.sh` copies `$SOURCE_DIR/out/deploy/EngineSDK` (a sibling of
   `$DEPLOY_DIR`, like AppAdvanced) to `Resources/EngineSDK` next to the `SDK/` block, before
   the signature. A missing folder only warns. On Windows/Linux, ci.yml moves
   `package/EngineSDK` into `package/Editor/`.
3. **Release set:** CI configures a `HE_BUILD_PROFILE=release`, `HE_BUILD_TESTS=OFF` tree from the
   same checkout (same `DEPLOY_DIR`/`HE_ENGINE_SDK_DIR`) and builds `--target HorizonSDK` before
   packaging. This is analogous to `build_runtimes.py`, maybe as one more mode there, and costs
   about 5-9 min per platform, push only. Locally it stays a documented recipe. Without this,
   Shipping exports keep falling back. Its own step if step 2 gets too big.
4. **Rejected: on-demand build in the export worker.** `cmake --install --component HorizonSDK`
   needs the engine build tree, and a packaged editor has none. In a dev tree it would only do
   what item 1 does automatically, and a release set would mean configuring and building a whole
   second tree inside an export.

## Human's question (#135): one SDK folder for both?

If "both" means dev + release: that is already the case (one `EngineSDK/`, `lib/dev` +
`lib/release`, one `include/`).
If "both" means `SDK/` (HorizonCode codegen) + `EngineSDK/`: possible, but its own step after the fix:
* `SDK/` (24 MB) = HE_Core/HE_Scene headers in **checkout layout** (`src/HE_*/include`) + glm +
  the editor's **split** `libHorizonCore`/`libHorizonScene`. In-editor "Build and Reload"
  (`GameLogicBuildPanel.cpp:95`) loads the dylib into the editor process, so it must link these
  libs, not the bundle. `engineRootFromSdk` (`HcCodegen.cpp:5133`) needs exactly that layout.
* `EngineSDK/` = flat `include/` + `lib/<profile>/libHorizonEngine`. `sdkFromRoot`
  (`HcCodegen.cpp:4424`) already accepts a root with the package. Since 211 step 13 the exported
  game's HorizonCode builds against `EngineSDK` anyway.
* Conflicts to resolve: the editor POST_BUILD `rm -rf <editor>/SDK` (`src/HE_Editor/CMakeLists.txt:642`,
  which is why `HE_ENGINE_SDK_DIR` must never be `<editor>/SDK`), and the two include layouts.
  Possible shape: `EngineSDK/lib/editor/` for the split libs, generator/engineRoot on the flat `include/`.
  It saves about 6 MB of duplicate headers and one folder. It touches `resolveSdk`, `engineRootFromSdk`, the
  POST_BUILD, `package_macos.sh` and `test_export_sdk_build`.

## Handoff

- **Topic / step:** 222, step 1 (finding, no code). This doc only.
- **Done:** cause found (table above), reproduced on macOS from the build graph, the deploy folder and an isolated
  `package_macos.sh` run. SDK install cost measured. Fix approach chosen (items 1-3, item 4 rejected).
- **Verified:** everything under "Reproduced", on this Mac against the main checkout's `cmake-build-release`
  (read only; the `install_manifest_HorizonSDK.txt` my measurement wrote was deleted again).
- **Not verified:** a fresh from-scratch build; Windows/Linux (static from ci.yml); release-tree build time on CI.
- **Decisions taken:** fix = dependency + package copy + release tree in CI. No on-demand build in the export.
  Merging `SDK/` into `EngineSDK/` is suggested only, and not in step 2.
- **Fallen:** the branch base (see top). zsh: `git show "$R:src/..."` treats `:s` as a modifier, so write `"${R}:path"`.
  `package_macos.sh` always writes `$SOURCE_DIR/out/HorizonEditor-<ver>.dmg`, so do not run it in the main checkout
  (it would overwrite the human's dmgs). Use the /tmp setup above.
- **Next steps:** 0. Queen: rebase this branch onto `release/0.7.0` (or retarget) before step 2.
  1. Step 2: item 1 + item 2, measure incremental `--target HorizonEditor` before/after, export dialog
  checkbox for Development. 2. Release set in CI (item 3), as part of step 2 or as step 3.
  3. Optional step: one SDK folder (#135).
