# Every export compiles its game from the engine SDK (topic 222, step 3)

Before this step "Compile game from SDK" was a checkbox: off by default, and when it was on,
anything that went wrong (no SDK, no cmake or compiler, a failed compile) quietly left the
prebuilt game from the editor's `Game/` runtime in the export. Since this step there is no
option and no fallback: every export compiles its game from the engine SDK, and an export
that cannot do that does not start, or fails.

## What an export does now

| Situation (Host target) | Before | Now |
|---|---|---|
| SDK + toolchain present | prebuilt game unless the box was ticked | game compiled from the SDK (`Compile game` step, always present) |
| compiler or cmake missing | prebuilt game, yellow line | Export **parks**: Tool Setup opens on the missing row with "An export is waiting..." and the run starts by itself once the probe finds both tools. Closing Tool Setup without them cancels it (notification) |
| probe still running | treated as "present" | Export parks until the probe answers |
| no SDK with the profile's lib set (e.g. Shipping, no `lib/release`) | prebuilt game | Export button disabled, line says why; the MCP door refuses with the reason |
| target Windows/Linux/macOS by name | prebuilt runtime of that platform | blocked: "export the <X> build from the editor there" (macOS on a Mac: "pick Host") |
| compile fails / is cancelled | prebuilt game stays | export **fails**, the runtime's executable is deleted from the folder, Start Game is greyed out |

The runtime bundle (`Game/`) is still copied first as the export's scaffolding (data layout,
`libHorizonPython`, the key the exporter patches in and `writeBuildConfigFromExport` reads back).
Its executable and engine libraries are replaced every time; it never ships as the game.

## Where

* `src/HE_Editor/ExportSdkBuild.h` / `.cpp`: `checkExportGate()` (Ready / Waiting / NeedTools /
  Blocked; Blocked wins over missing tools) and `codegenSdkFor()` (replaces `chooseCodegenSdk`:
  compiled HorizonCode always links the game's SDK).
* `src/HE_Editor/ExportDialogPanel.cpp`: `currentExportGate`, `failBeforeStart`, `requestStart`
  (button / Build Again), `pumpWaitingExport` (every frame while parked), `startExport` (gate as
  backstop), `startFromProfile` (MCP: refuses instead of parking), `compileGameFromSdk` (returns
  `SdkGameResult`, deletes the executable on failure). Checkbox, `s_exportSdkGame` and the
  HorizonCode toolchain/`<editor>/SDK` disabling are gone.
* `src/HE_Editor/ExportSdkGateUi.{h,cpp}` (new): the dialog's line, its own file so he_tests draws it.
* `src/HE_Editor/ToolSetupDialog.{h,cpp}`: `requestShowFor(tool, purpose)`, `purpose()`, `isShowing()`.
* `src/HE_Editor/EditorApplication.cpp` `startToolchainProbe`: `HE_SIMULATE_MISSING_TOOLCHAIN=compiler|cmake|both`.
* `ExportProfile::compileGameFromSdk`, the MCP `project_package` argument and its readbacks are
  removed. An old `.heproj` with the key loads; the key is dropped on the next save.
* `scripts/package_macos.sh`: missing `out/deploy/EngineSDK` is now an error (exit 1), not a warning.
* `EditorHelp.cpp`: entry `Export/Compile game from SDK` removed; Compile HorizonCode, Development
  hotkeys and Build Window/Cancel texts updated.

## Verification (macOS, M5 Air, this worktree's `out/build/release` + `out/deploy`)

| Check | Result |
|---|---|
| `cmake --build out/build/release --target HorizonEditor he_tests -j 8` | green |
| `he_tests --source-file=*test_export_sdk_build.cpp` (new: `checkExportGate`, `codegenSdkFor`) | 12/12 |
| `*test_export_profiles.cpp` (new: old `compileGameFromSdk` key loads, dropped on save) | 51/51 |
| `*test_mcp_tools_build.cpp` | 13/13 |
| `*test_tool_setup_ui.cpp` (new: export line with/without compiler, Tool Setup opened for the export) | 16/16 |
| `scripts/editor_help_audit.py --check` | 0 missing (export 38/38) |
| `scripts/he_sdk_export_live.py /tmp/he222s3_live` (live editor, MCP) | `Development=True Shipping=True no_compiler=True` |

Live witness details: Development exported in 50 s, "Game compiled from the SDK (dev)", and the
exported `HorizonTutorial` is byte-identical to the SDK build artifact and differs from
`out/deploy/Game/HorizonGame`. Shipping was refused before start ("no engine SDK with the 'release'
lib set"), nothing written. With `HE_SIMULATE_MISSING_TOOLCHAIN=compiler`, `project_package` was
refused ("no working C++ compiler found; install it through the editor's Tool Setup") and no export
folder appeared.

Dialog check without and with a (simulated) missing compiler, headless frames from
`HE_UI_DUMP_DIR=... he_tests --source-file=*test_tool_setup_ui.cpp`: `export-sdk-toolchain-ok.bmp`
(grey "Game compiled from the engine SDK (dev): <path>"), `export-sdk-no-compiler.bmp` (orange
sentence + "Set Up..."), `export-tool-setup-waiting.bmp` (Tool Setup with "An export is waiting..."
above the rows, compiler row flashed with Install).

Full `HOME=<scratch> he_tests --source-file-exclude=*test_material_graph.cpp`: 5493/5495; the two
failures are `test_profiler.cpp:107/215`, order-dependent and known from step 2 (the file alone passes).

## Handoff

- **Topic / step:** 222, step 3 of 4
- **Branch / commit:** `claude/engine-sdk-wird-nicht-mitgebaut-gepackaged-horizonsdk-target-v2` @ 69fef267, df5f7965, 16cbf74f + this doc
- **Done:** every export compiles its game from the engine SDK. No checkbox, no profile field, no MCP
  argument, no prebuilt fallback. Missing toolchain: Export parks, Tool Setup opens with the reason, the
  run resumes by itself. No SDK / non-Host target: refused up front. Failed compile: export fails and
  the executable is deleted.
- **Files touched:** `ExportSdkBuild.h` (`checkExportGate`, `codegenSdkFor`), `ExportDialogPanel.cpp`
  (`currentExportGate`, `requestStart`, `pumpWaitingExport`, `failBeforeStart`, `compileGameFromSdk`,
  `startFromProfile`), `ExportSdkGateUi.*` (new), `ToolSetupDialog.*` (`requestShowFor`, `isShowing`).
- **Verified:** see the table above (build, 4 test files, help audit, live editor via MCP, headless frames).
- **Not verified:** the button's park-and-resume path in a real editor window (`pumpWaitingExport`):
  no GUI clicking on this machine, and the panel is not linked into he_tests. Its pieces are tested
  (gate, `requestShowFor`/`isShowing`, the banner click), the state machine itself only by reading.
  A real toolchain install. Windows/Linux (CI).
- **Decisions taken (queen confirmed, request 138):** Shipping without `lib/release` is refused (no
  fallback) until step 4; non-Host targets are blocked; `package_macos.sh` exits 1 without EngineSDK.
  The MCP door refuses instead of parking (nobody there to press Install). Tool Setup is offered, its
  installer is not auto-started (the click is the consent for a password/UAC prompt). The `Game/` runtime
  stays as scaffolding (Python plugin, data layout, patched key read back by `writeBuildConfigFromExport`).
- **Decisions suggested, not taken:** ship a runtime bundle without `HorizonGame` in the editor package
  (`isRuntimeBundle` requires the exe today); merge `SDK/` (codegen) and `EngineSDK/` (#135), now that
  the codegen SDK is no longer linked into exports (`HE::hccg::resolveSdk(base)` unused by the export).
- **Traps:** an empty `NewFrame/Render` (like `toolSetupOpen()`) right before a headless click makes
  ImGui take the press as a window click; run two frames over the item first. Errors from the MCP
  client arrive as the repr of a content list (quotes escaped). `cmake/HorizonSDK.cmake:245` still has
  a comment about "falling back to the prebuilt game" (left alone to avoid a reconfigure mid-build).
- **Next steps:** 1. Step 4: release-profile tree in CI and in local packaging (`HE_BUILD_PROFILE=release`,
  `HE_BUILD_TESTS=OFF`, `--target HorizonSDK`, same `HE_ENGINE_SDK_DIR`), then rerun
  `scripts/he_sdk_export_live.py` (it expects Shipping to compile once `lib/release` is beside the editor).
  2. Click through Export with `HE_SIMULATE_MISSING_TOOLCHAIN=compiler` in a visible editor once a human is
  at the machine (park, Tool Setup, Close cancels with a notification).
