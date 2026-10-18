# Export from an engine SDK: survey of runtime profiles, config values, dev branches and today's HorizonCode codegen (Thema 211, step 1, 2026-10-18)

Survey only, no code changed. Every claim has a `file:line` from HEAD `2a63303d`.
Earlier related record: `docs/hc-codegen-sdk-export-befund-2026-10-02.md` (codegen SDK
staging, `resolveSdk`). The plan for the codegen itself is `docs/horizoncode-cpp-codegen-plan.md`.

## Summary

- **`HorizonGame` knows no build profile at runtime.** `ExportProfile` (`src/HE_Tools/src/FileOps/ProjectManager.h:183`,
  defaults "Development" and "Shipping" at `ProjectManager.cpp:617/624`) is split up at export time into separate
  switches (`devTools`, `encrypt`, `compress`, `enableModSupport`, `compileHorizonCode`). The binary only
  sees `devTools`, as hcfg bit 4096, and it decides exactly two keys (V, F10). There is no `HE_SHIPPING`,
  `HE_DEV` or `isShipping` anywhere.
- **The exporter never picks a build type.** It copies whatever runtime is in `DEPLOY_GAME`
  (`ProjectExporter.cpp:944-983`). So a Debug or RelWithDebInfo runtime carries `HE_GAME_DEV_HOTKEYS` into a
  "Shipping" export (`src/HE_Game/CMakeLists.txt:51-55` says so itself).
- **The pak key is patched into the copied exe** (a 64-byte magic block), and the exe is then re-signed ad hoc.
  Without the block, the key falls back to plain text in `project.hcfg`.
- **Three files steer the game:** `project.hcfg` (binary), `config.json` (render and window settings) and
  `Config/ProjectSettings.json`. In addition about 60 `HE_*` environment variables are read; none of them is profile-gated.
- **Dev features are almost all always compiled in:** F9 profiler capture, the capture/dump automation, the
  free-fly fallback camera, the debug lines, the env overrides `HE_CONFIG_DIR`/`HE_LOG`.

## A. HorizonGame and the app runtimes today

"App runtimes" are the same `HorizonGame` binary in flavours `game`, `app-advanced` and `app-basic`
(`HE_RUNTIME_FLAVOR`, root `CMakeLists.txt:671-688`). The flavour only sets the backend list at compile time,
and the exporter picks it per project (`runtimeFlavorFor`, `ProjectExporter.cpp:60-62`). App mode
itself is a runtime flag in hcfg (bit 16, peeked in the constructor at `GameApplication.cpp:464-469`).

### A.1 Table: value or feature, today's path, target

"Target" is the proposal for steps 2 to 4 (`HE_PROFILE_DEV` / `HE_PROFILE_RELEASE`, generated header,
called `HEGameConfig.h` here). Nothing of it is decided or built yet.

| Value / feature | Today (file:line) | Target |
|---|---|---|
| Build profile | none at runtime; export profile → separate switches (`ExportDialogPanel.cpp:405/453`, `es.devTools = s_exportDevTools && !es.appProject`) | `HE_PROFILE_DEV` or `HE_PROFILE_RELEASE` as a compile definition of the game build; logged at start like the flavour (`:587`) |
| Dev hotkeys V (VSync) and F10 (stats card) | `HE_GAME_DEV_HOTKEYS` from `$<CONFIG:Debug,RelWithDebInfo>` or `option` (`HE_Game/CMakeLists.txt:48-59`) **OR** hcfg `devTools` at runtime (`GameApplication.cpp:601-603, 657`); keys at `:2890-2916`, card `:2963-2972` | `#if HE_PROFILE_DEV` only; the hcfg bit and the runtime OR go away in the SDK path (they stay for the prebuilt path) |
| Pak key (AES-256-GCM) | `g_hePakKeyBlock` (`EmbeddedPakKey.h/.cpp`, magic `HE_EMBEDDED_PAKKEY_V1`) patched into the copied exe (`ProjectExporter.cpp:148-154, 220, 1036-1056`), ad-hoc codesign `:207-215`; fallback plain key in hcfg `encKey` (`:1175-1178`); read `GameApplication.cpp:735-737` | `constexpr` key in `HEGameConfig.h`, no patching and no re-signing; the patch path stays for the prebuilt fallback. Key reuse across exports (`ProjectExporter.cpp:622-647`) must read the previous key from the export folder, not from the exe |
| `HE_PROJECT_SECRET` | defined in `HE_Game/CMakeLists.txt:61-64`. No source file reads it; only `CopilotDocs/HorizonGame.md:58,371` describes it as a design (a secret for the pak key) | remove, or take over its role in `HEGameConfig.h` |
| Startup scene UUID, hpak file name, project name, main scene name | `project.hcfg` (binary, `HCFG`, `k_version=7`, `ProjectConfig.cpp:8,22`; written `ProjectExporter.cpp:1134-1189`); read `GameApplication.cpp:656`, scene `:746-749, :1317` | `constexpr` in `HEGameConfig.h`. Then the lesson trap "foreign hcfg → black frame" goes away too |
| hcfg flags: modSupport 1, encrypted 2, hasPackedScene 4, horizonCodeCompiled 8, appMode 16, !advancedShaderEffects 32, allowFiles/Processes/Network 64/128/256, fontScripts 512-1024, !fontBold 2048, devTools 4096 (`ProjectConfig.cpp:61-85`) | runtime `if` at each use: mods `:761-763`, permissions `:935-937`, fonts `:944-945`, theme `:859-872`, HC compiled warning `:835` | `constexpr bool` in the header (permissions, appMode, mods); `if constexpr` lets the compiler drop unused paths. `advancedShaderEffects` is not read by the game at all (only the flavour choice uses it) |
| hcfg tails: save template, theme, bundleId, collision layers JSON, audio buses JSON (v3-v7) | read at `:949, :859, :1076, :2107, :1396` | JSON as a string constant or as a generated table in the header; low priority (data, not a dev feature) |
| `config.json` (window size/mode, VSync, backend, PauseOnFocusLoss, Bloom/SSAO/DoF/MotionBlur, GI*, SSR*, AA, RenderScale, RenderPath, GpuParticles) | `GlobalState::useShippedConfig(SDL_GetBasePath())` (`main.cpp:22-23`, `GlobalState.cpp:150-157`); keys `GameApplication.cpp:489-536, 3328-3529`; written `buildGameConfigJson` (`ExportDialogPanel.cpp:148-232`) + `writeGameConfig` (`ProjectExporter.cpp:1191-1250`) | **Defaults** go into the header (so the game starts without the file). The file stays as an optional player override, because these are player settings (`HE::api::settings`) and must remain changeable |
| `Config/ProjectSettings.json` (title, splash, deadzones, shadows, physics Hz/gravity/threads/budget, anti-cheat telemetry URL) | `HE::loadProjectSettings` (`GameApplication.cpp:477`), copied verbatim (`ProjectExporter.cpp:1256-1283`) | into the header as constants (author settings, not player settings) |
| Splash (`Splash.png` + `splashEnabled`) | `GameApplication.cpp:555-578`, with no profile branch. The SplashScreen's built-in engine branding (no logo) is deliberately unreachable from the game. The lessons tie a ~10 s logo intro to Development exports, but the code shows no profile dependence and this step did not reproduce it | `constexpr bool kSplash`; image stays a file or is embedded (open) |
| `logVerbosity` / `HE_LOG` / `HE_LOG_NO_CONSOLE` | `GlobalState.cpp:76-83, 314-317`; `buildGameConfigJson` does not write `logVerbosity` | default verbosity per profile in the header; `HE_LOG` only in DEV |
| `HE_CONFIG_DIR` | `GlobalState.cpp:174`, outranks the shipped pin on purpose | DEV only |
| F9 profiler capture, `HE_PROFILE_*`, `HE_CAPTURE_*`, `HE_PERF_*` | `Application.cpp:294-298, 475-590, 77, 407`; **engine-wide, not gated**. `HE_PROFILING` (root `:801`) only removes scope macros | `#if HE_PROFILE_DEV` around the F9 key and the env automation. This sits in HE_Core, so the SDK needs a DEV and a RELEASE variant of the lib, or the check moves to the game (open, step 2) |
| Headless/test hooks `HE_HIDDEN_WINDOW`, `HE_EXIT_AFTER_FRAMES`, `HE_DUMP_PATH`, `HE_BACKGROUND_FPS`, `HE_NO_SPLASH`, `HE_SPLASH_DUMP` | `Window.cpp:69-71, 120-122`, `Application.cpp:464`, `SplashScreen.cpp:74,392` | DEV only. Caution: perf and image scripts (`scripts/perf/*`, `scripts/he_vk_imagetests.py`, `docs/headless-runs.md:209`) drive Release-built binaries with these. So these hooks must stay in the prebuilt runtime (which is a dev build), and only the SDK RELEASE profile drops them |
| Renderer debug env (`HE_RENDER_PATH`, `HE_DUMP_GBUFFER`, `HE_SKY_TIME`, `HE_GI_FORCE_SW`, `HE_GPU_DEBUG`, `HE_GL_DEBUG`, `HE_FOLIAGE_*` …) | in the backends (grep only; whether an enclosing `#ifdef` exists was not checked per site) | out of scope (renderer backends are not part of the topic); list only |
| Physics tuning env `HE_PHYSICS_THREADS/WATER/BUDGET_MS`, `HE_PROPAGATE_FULL` | `PhysicsTuning.h:72,167`, `PhysicsBudget.h:90`, `TransformHierarchy.cpp:~245` | DEV only, after step 2 decides how HE_Scene/Core see the profile |
| Net env `HE_NET_LOOPBACK_ONLY`, `HE_NET_RELAY` | `Socket.cpp:146`, `RelayTransport.cpp:127` | not verified whether players need `HE_NET_RELAY`; ask before gating |
| Free-fly fallback camera | `ensureDefaultCamera` `GameApplication.cpp:1385-1388`, controller `:3231` (`if (!m_appMode)`) | DEV: keep; RELEASE: undecided. It is also what makes a scene without a camera visible at all |
| Debug lines (`api::debug::collect`) | `GameApplication.cpp:3155-3159` | script API (`HE::api::debug`), keep in both; can be stripped in RELEASE later |
| HorizonCode compiled or interpreted | dylib `HorizonCodeGen` loaded `:833`, misses fall back to the interpreter `:832-839, 901-907, 1218` | keep the fallback (topic: the path without a compiler must work). In the SDK path the generated code can be linked into the game statically, with no dylib |
| GameLogic native lib | loaded if the file exists `:1421-1432` | in the SDK path, link it into the game directly; the dylib stays for the prebuilt path |
| Lua/Python | Python plugin shipped per project (`HE_Game/CMakeLists.txt:96-108`, `ProjectExporter.cpp:902-962`) | unchanged (decided by the files that ship, not by the profile) |
| Integrity probe / anti-cheat | `:783-797` runs whenever a pak is mounted | unchanged |
| `HE_EDITOR` on `HorizonCore` | `HE_Editor/CMakeLists.txt:490` adds it PRIVATE to `HorizonCore`. The comments at `HE_Game/CMakeLists.txt:68-70` and root `:896-898` say the game DLLs come from a separate build without `HE_EDITOR`. Whether the game-flavour tree (which also builds HE_Editor, root `:948-949`) really keeps it out was **not checked**. No effect today (0 `#ifdef HE_EDITOR` in Core/Scene/Rendering) | open question for step 5: the SDK's `HorizonCore` must come from a tree without `HE_EDITOR` |

### A.2 Export pipeline today (where the SDK path will branch)

`ExportDialogPanel.cpp`: profile statics `:67-72`, combo `:588-646`. `findRuntimeBundle(base, platform, flavor)`
`:1462-1465` picks the prebuilt runtime, worker thread `:1714`, `ProjectExporter::exportProject` `:1943`.
`ProjectExporter.cpp:1470-1500` runs the phases in order: layout, pack, binaries (`copyRuntimeBinaries` `:891-1100`,
key patch `:1036`), HorizonCode lib (`:1112-1130`), hcfg (`:1134`), config.json, ProjectSettings, splash, icon, bundle
and codesign (`:1439-1455`). The SDK compile replaces "binaries + key patch + hcfg"; everything else stays.

## B. HorizonCode codegen today

### B.1 What gets generated

`hc_codegen --fixtures --out DIR` (built `cmake-build-release`, Oct 15; `HcCodegen.cpp`,
`HorizonCodeGenSupport.h` and `HcCodegenCli` have no commits since) writes 106 files for
the test fixtures:

| File | Content |
|---|---|
| `hcgen_C_<key>.h/.cpp` | one class per graph, `class C_<key> final : public HorizonCode::CompiledInstance` |
| `hcgen_type_S_<Name>.h`, `E_<Name>` | user structs / enums (`S_FixStats`, `E_FixMood`) |
| `hcgen.h` | umbrella include of every class and type |
| `hc_registry.h/.cpp` | manifest `hcgen::classes(int*)` (key → `create_C_*`/`destroy_C_*`), `engineVersion()` (`"dev"`), and the single C export `HE_HorizonCodeGenClasses` under `HCGEN_BUILD_DYLIB` |
| `CMakeLists.txt` (export build only, `generateCMakeLists` `HcCodegen.cpp:4062-4093`) | `add_library(HorizonCodeGen SHARED …)`, hidden visibility, links **only `HorizonCore`** (`:4087`) |

Who calls it: the CLI (`src/HE_Tools/src/HcCodegenCli/main.cpp`, 95 lines) is used only by
`tests/CMakeLists.txt:1122` to make the parity fixtures. The export runs the library in-process:
`ExportDialogPanel.cpp:1737-1754` (`generate`), `:1822` (`generateCMakeLists`), `:1847-1848`
(`resolveSdk`, `buildDylib`). The game loads the dylib through `CompiledClassTable`
(`HcCompiledLoader.cpp:25`, symbol `HE_HorizonCodeGenClasses`) at `GameApplication.cpp:833`.
Interpreted fallback: if the dylib is missing or rejected (`GameApplication.cpp:830-838`) or a class key is not in it (`:1218`), the runtime
interprets that graph.

### B.2 Names: what is readable and what is cryptic

Readable already: the class (`C_GameInstance`, `C_<file stem>`), types (`S_FixStats`,
`E_FixMood`, fields keep their names), and the **public wrappers**. Each event (single,
unfiltered, non-engine), each function and each public variable gets one under its authored
name (`void AddScore(float amount, float& total)`, `float& score()`; `HcCodegen.cpp:1510-1533`).

Cryptic or redundant (identifier builders `HcCodegen.cpp:508-548`, `buildNames` `:1470-1534`):

| What | Today | Why it is in the way |
|---|---|---|
| Prefixes | `v_score`, `f_AddScore`, `l_tmp`, `ia_Jump_7`, `kEvt_X` | the prefix carries the kind of the name, not the node's meaning |
| Event bodies | `ev_Tick_13`, `ev_OnClicked_1` / `ev_OnClicked_6` | the node id leaks into the name |
| Function bodies | `f_AddScore(RunState& rs, float p_0, float& r_0)` plus a forwarding wrapper with the real names | two functions per graph function, positional parameter names |
| Temporaries / cache | `a3`, `r3`, `rs.n5_o0`, `n9_state`, `resume_7` | node ids, no pin names; `r<id>` is emitted even when nothing reads it (`:2263`) |
| Two entry paths per event | typed hook override (`onTick(float)`) **and** an `if (name == "Tick")` chain in `fireEvent` (`:3488-3562`) | same body, two dispatchers |
| Reflection boilerplate | `slots()` (`hc::slot<&C::v_x>("x", hc::PinType::Float, false, 0, "", …)`), `varInfos`, `funcInfos` with `kFnP_f_X[] = { (HorizonCode::PinType)1 }`, `classTag_` = address of a static char | needed by the runtime's name-based paths (savegames, Bind To, `callFunction` from interpreted graphs), but the casts and positional flags cannot be read |
| Factory glue | `create_C_X` / `destroy_C_X`, `kClasses[]`, `#if defined(HCGEN_BUILD_DYLIB)` | needed for the dylib; does not belong in the class file |
| Stale comments | "bodies inline" (`:1005-1010`), "there are no generated headers" (`:3994-3995`) | wrong since bodies moved to the .cpp |

What must stay (the runtime needs it, and the dispatchers make compiled and interpreted
classes interoperate in one Runtime, parity case `:1745`): `classKey`, the slot table (`get/setVariable`, `reseed`,
`collectRefs`), `callFunction` by name, the CompiledInstance hooks, and the C manifest export. These can
move to a generated `*_reflect.cpp` next to the class instead of sitting between the logic.

### B.3 Engine-API calls: a string dispatcher, not a direct call

Every engine node goes through `hc::callApi(m_ctx, "<id>", std::vector<hc::Value>{…})`
(pure: `HcCodegen.cpp:1885-1898`, re-dispatched on every read; exec: `:2707-2712` via
`emitValueCall` `:2248-2274`). The fixture output has 87 such calls on 82 lines. The path at runtime:

1. `hc::callApi` (`HorizonCodeGenSupport.h:712`) → `Context::callApi` (std::function, `HorizonCodeRuntime.cpp:1498`)
2. → `Services::callApi` (std::function, `GameApplication.cpp:1275`; the editor has a twin)
3. → `HE::api::find(id)` (hash lookup, `EngineApi.cpp:8895`) → `ApiFn::invoke` (std::function, Value marshalling)
4. → the real function, e.g. `HE::api::animator::setParam(Ctx&, …)`.

So one engine call means three `std::function` hops, one string hash, boxing and unboxing of every argument
and a `std::vector` per call. Unknown ids warn and are skipped at runtime (`GameApplication.cpp:1281`).

### B.4 What is already usable as an engine-API header

`src/HE_Scene/include/HorizonScene/EngineApi.h` (3144 lines) is the natural basis:

- `namespace HE::api` with free functions over an explicit `Ctx&` and plain types (`glm::vec3`,
  `Entity = uint32_t`, `std::string`, `HE::UUID`) in 47 sub-namespaces (`entity`,
  `transform`, `physics`, `animator`, … `input`; overview `grep -n '^namespace' EngineApi.h`).
  The header's own design comment (`:32-51`) says it was written so the same signature can serve a direct call
  from generated C++.
- Registry: `struct ApiFn { id, category, isExec, params, results, cppCall, invoke, displayName }`
  (`:3076-3095`). There are 510 rows in `EngineApi.cpp` (`t.push_back({ "`). **`cppCall`** (e.g. `"HE::api::transform::setPosition"`)
  is staged for exactly this direct-call codegen, and "nothing emits it today" (`:3081-3086`).
  `tests/test_engine_api.cpp:111` keeps it honest: `HE::api::` prefix, one distinct callee per row.
  `displayName` ("Set Position") is the node title.
- Includes only HE_Core (`HorizonCode.h`, `HorizonCodeRuntime.h`, `Types/UUID.h`) and glm; engine
  classes are forward-declared. No `HE_API` macros, but `HorizonScene` builds with
  `WINDOWS_EXPORT_ALL_SYMBOLS ON` (`src/HE_Scene/CMakeLists.txt:138`), so the functions link on Windows too.

Gaps before generated code can call it directly:

1. **Link**: the generated CMake links only `HorizonCore`. `HE::api` lives in `HorizonScene`, so the
   SDK must ship its headers and import lib (plan §7.1 assumed this; `docs/horizoncode-cpp-codegen-implementation-plan.md:670-705`).
2. **Ctx**: generated code holds an `hc::Context` (`m_ctx`), not an `HE::api::Ctx`. The app builds
   the `Ctx` per call in `apiCtx(world, physics, content, self)` (`GameApplication.cpp:1289`, editor twin).
   A direct call needs a way to get one: e.g. a `Context::apiCtx` resolver, as plan §7.1 sketches.
3. **Marshalling**: wire types and C++ types do not match everywhere. Vec3 travels as a `Color`
   on the wire (`EngineApi.h:47-51`), arrays as parallel lists, and some rows wrap a call (`scene.showZone`).
   The row's `params` order matches `cppCall` (test above), but the type conversion happens today in
   `invoke`. An emitter needs a per-param marshal tag or C++ signatures that take pin types.
4. **Size**: `EngineApi.h` is 3144 lines and mostly comments. A generated-code-facing header could be
   the same file. A thin `HorizonEngine.h` umbrella is enough; no second API copy (one source of truth).
5. Rows whose `invoke` does more than forward (the design-time filter `designTimeCallApi`, unknown-id
   tolerance, the creator stack `CreatorScope` in `HorizonCodeRuntime.cpp:1501`) need a decision:
   keep those rows on `callApi` or move the extra logic into the API function.

### B.5 Parity tests

`tests/test_horizoncode_codegen.cpp` runs every fixture (`tests/fixtures/hcodegen/`, generated
by `hc_codegen --fixtures` at build time, `tests/CMakeLists.txt:1090-1144`) once interpreted
and once compiled (`ParityPair`, around `:300-440`) and compares host-call traces, variables
and properties. There are about 45 `codegen parity:` cases (flow, math, containers, events, engine rows
such as `animator_sync :1194` and `engine_exec_cached :1420`, latent, structs, bind, extract).
This is the safety net for step 7. It was **not run** in this step.

### B.6 Example: today next to the target

Real output, fixture `animator_sync` (`hc_codegen --fixtures`, reformatted only for blank
lines). Graph: event `Update` → Animator *Set Param* (self, "speed", 1.5); variable `wrote` ←
Animator *Get Param* (self, "speed").

Today (`hcgen_C_animator_sync.cpp`, the logic part; plus 30 lines of slots/infos/fireEvent/factory):

```cpp
void C_animator_sync::Update() { ev_Update_1(); }
float& C_animator_sync::wrote() { return v_wrote; }
// … classKey, classTag_, slots(), varInfos(), eventInfos() …
void C_animator_sync::fireEvent(const std::string& name, int, const hc::Value&)
{
    if (name == "Update")
    {
        ev_Update_1();
    }
}
void C_animator_sync::ev_Update_1()
{
    {
        std::vector<hc::Value> a3;
        a3.reserve(3);
        a3.push_back(hc::toValue(hc::fromValue<int>(hc::callApi(m_ctx, "entity.self", std::vector<hc::Value>{}), 0)));
        a3.push_back(hc::toValue("speed"));
        a3.push_back(hc::toValue(1.5f));
        const std::vector<hc::Value> r3 = hc::callApi(m_ctx, "animator.setParam", a3);
    }
    v_wrote = hc::fromValue<float>(hc::callApi(m_ctx, "animator.getParam", std::vector<hc::Value>{hc::toValue(hc::fromValue<int>(hc::callApi(m_ctx, "entity.self", std::vector<hc::Value>{}), 0)), hc::toValue("speed")}), 0);
}
HorizonCode::CompiledInstance* create_C_animator_sync() { return new C_animator_sync(); }
void destroy_C_animator_sync(HorizonCode::CompiledInstance* p) { delete p; }
```

Target (hand-written sketch, not generated; `api()` stands for whatever gap 2 above settles on):

```cpp
// AnimatorSync.cpp — generated from fix/animator_sync.hcode
#include <HorizonEngine/HorizonEngine.h>   // HE::api + the HorizonCode base
#include "AnimatorSync.h"

using namespace HE::api;

// Event "Update"
void AnimatorSync::Update()
{
    Ctx& ctx = api();
    animator::setParam(ctx, entity::self(ctx), "speed", 1.5f);   // Set Param
    wrote = animator::getParam(ctx, entity::self(ctx), "speed");  // Get Param
}
// Reflection (slots, eventInfos, fireEvent by name) and the factory are in
// AnimatorSync.reflect.cpp; the dylib manifest stays in hc_registry.cpp.
```

The rules this sketch implies, for step 7:

- class name = graph name in PascalCase without `C_`;
- members = variable names without `v_`;
- one function per event or function, with authored parameter names and no `ev_<id>` / `f_` split;
- engine nodes are called as `cppCall` with typed arguments;
- temporaries are named after the node's display name and pin (`speed`, `hitResult`), not `a3`/`r3`;
- reflection and factory go into a second file.

Whether `RunState` / `HC_STEP` (step limit 4096, `HorizonCodeGenSupport.h:1020-1031`) stay in the readable
body or move behind a macro is open. They are needed for parity with the interpreter's step limit.

## Handoff

- **Topic / step:** Thema 211, step 1 of 9 (survey)
- **Branch / commit:** claude/export-sdk-compile-horizoncode-codegen @ 11dabde1 + the fix-up commit after it (pushed: yes)
- **Done:** inventory of everything the game reads at runtime (profile, pak key, hcfg, config.json,
  ProjectSettings, env) with a target per value (A.1), the export pipeline's branch point (A.2), the
  codegen output and its cryptic parts (B.2), the engine-API call chain (B.3), what in `EngineApi.h`
  already works as a header and what is missing (B.4), and a real generated example next to the target (B.6).
- **Files touched:** this doc only. No code.
- **Verified:** `hc_codegen --fixtures` (main checkout `cmake-build-release`, Oct 15; no codegen commits
  since) produced the B.6 example and 82 `callApi` sites. Counts were checked by grep: 510 registry rows,
  47 `HE::api` namespaces, 0 `#ifdef HE_EDITOR` in Core/Scene/Rendering, no source file reads `HE_PROJECT_SECRET`, and F9/`HE_PROFILE_CAPTURE` have no enclosing `#if`
  (`Application.cpp:294-298, 475-590`). The splash has no profile branch (`GameApplication.cpp:555-578`).
  `HE_GAME_DEV_HOTKEYS`, the key choice (`GameApplication.cpp:735-737`) and `callApi` (`:1275`) were read at the source.
- **Not verified:** the parity tests (`test_horizoncode_codegen.cpp`) were not run. Per-site `#ifdef`s around the
  renderer env variables were not checked. Whether players need `HE_NET_RELAY` is unknown. The B.6 target is a
  hand-written sketch, not compiled.
- **Decisions taken:** none (survey). The names `HE_PROFILE_DEV`/`HE_PROFILE_RELEASE` and `HEGameConfig.h` are placeholders.
- **Decisions suggested, not taken:**
  1. `config.json` stays as an optional player override, and its defaults go into the header.
  2. The prebuilt runtime stays a dev build with all hooks; only the SDK RELEASE profile strips them.
  3. Direct engine calls emit `ApiFn::cppCall` and keep `callApi` only for rows whose `invoke` does more than forward.
  4. Reflection and factory move into a separate generated file.
  5. The SDK links generated code and GameLogic statically into the game.
- **Pending:** steps 2-9 as on the board.
- **Traps:**
  - Dev features in HE_Core (F9, `HE_PROFILE_*`, the headless hooks) cannot be stripped by a game-only define. Step 2 must
    decide between a define on the SDK libs (two lib variants) and a runtime switch that the game sets once.
  - The editor's CMake adds `HE_EDITOR` to `HorizonCore` (`HE_Editor/CMakeLists.txt:490`); whether the
    game tree stays clean is open (A.1, last row).
  - Generated code links only `HorizonCore`; `HE::api` lives in `HorizonScene`.
  - `hc_codegen` CLI writes no CMakeLists; the export does (`generateCMakeLists`).
- **Next steps:**
  1. Step 2: define the profile defines in root CMake and settle the "dev code in HE_Core" question (A.1, F9 row).
  2. Step 4: list the `HEGameConfig.h` fields from A.1 (hcfg fields + ProjectSettings + config.json defaults).
  3. Steps 6/7: add a `Context` → `HE::api::Ctx` resolver (B.4 gap 2) and a per-param marshal rule (gap 3), then emit `cppCall`.

## Handoff (step 2: profile definitions)

- **Topic / step:** Thema 211, step 2 of 9. Branch claude/export-sdk-compile-horizoncode-codegen, commits ee7a583a, e5f8355b + the doc commit after them.
- **Done:** `cmake/HorizonBuildProfile.cmake` (cache `HE_BUILD_PROFILE=dev|release`, `he_target_build_profile(<t> [scope] [PROFILE p] [FEATURES N=0|1])`,
  feature-name list `HE_BUILD_FEATURES`), included from root `CMakeLists.txt` after the `HE_PROFILING` block.
  `src/HE_Core/include/Platform/BuildProfile.h`: `HE_BUILD_PROFILE_DEV/RELEASE` (both always defined), four `HE_FEATURE_*`
  switches with their per-profile defaults, `HE_DEV_ONLY`/`HE_RELEASE_ONLY`, `HE::kDevBuild/kReleaseBuild/kBuildProfileName`.
  Rules: `docs/coding-conventions.md` section 9. Test: `tests/test_build_profile.cpp` + `tests/build_profile_probe.{h,cpp}`,
  compiled into four static probe libs (dev, release, no call, release + `TEST_HOOKS=1`) in `tests/CMakeLists.txt`.
- **Decisions taken:** names `HE_BUILD_PROFILE_*` instead of the placeholder `HE_PROFILE_*` (that is the profiler's family);
  dev code in the engine libs is stripped at compile time with one library set per profile (one build tree each), applied
  `PUBLIC` on the libs, no runtime switch; no definitions = dev (`HE_BUILD_PROFILE_DEFAULTED 1`); feature defaults live only
  in the header, CMake only overrides. No engine target calls the function yet.
- **Verified:** `he_tests --source-file='*test_build_profile.cpp'` 4 cases / 42 assertions (also via ctest). Mutation
  (`HE_DEV_ONLY` passing through in release) turns 2 cases red. `clang++ -fsyntax-only -Wundef` of the probe under six
  definition sets, `#error` for both-1 and both-0, C mode. Scratch CMake project: defaults, `PUBLIC`, overrides, and the
  five configure errors (bad cache value, bad `PROFILE`, unknown feature, value not 0/1, stray argument). The incremental
  rebuild after the change compiled only the probes and the test (13 steps). Full `ctest -j4`: 303/304; the one red,
  `test_hpak` (`:1188`, 6 == 2), comes from the 4 assets in this Mac's real `EngineContentCache` and passes with a private
  `HOME` (Hive lesson 415), not from this step.
- **Not verified:** MSVC and Linux compile (CI started at the end of the step). No target compiles with `-Wundef`, so a misspelt
  `#if HE_FEATURE_X` is silently 0.
- **Traps:** do not apply the profile with `add_compile_definitions` (the `HE_PROFILING` pattern): the "default" probe must
  receive no definitions. The probes mix profiles in one binary; that is safe only because they include the macro header alone.
- **Next steps (step 3):** call `he_target_build_profile(<lib> PUBLIC)` on HorizonCore/Scene/Rendering/Net and the game;
  replace `HE_GAME_DEV_HOTKEYS` + hcfg `devTools` (prebuilt path keeps the hcfg bit) with `HE_FEATURE_DEV_HOTKEYS`; put F9 /
  `HE_PROFILE_*` behind `HE_FEATURE_PROFILER_CAPTURE`, the A.1 headless hooks behind `HE_FEATURE_TEST_HOOKS`, `HE_CONFIG_DIR`/`HE_LOG`/physics
  env behind `HE_FEATURE_ENV_OVERRIDES`; decide whether release forces `HE_PROFILING` off; consider `-Wundef` on the gated targets.

## Handoff (step 3: dev features behind the profile)

- **Topic / step:** Thema 211, step 3 of 9. Branch claude/export-sdk-compile-horizoncode-codegen, code commit 6cb54243 + the doc/script commit after it.
- **Done:** `he_target_build_profile` on HorizonCore/Scene/Net (PUBLIC, `src/HE_*/CMakeLists.txt`) and HorizonGame (PRIVATE);
  the renderer libs inherit it through Core, their CMake and code are untouched. Gated, by switch:
  - `DEV_HOTKEYS`: V/F10, the hcfg `devTools` door, `m_devStats`/`m_devHotkeys` (`GameApplication.cpp` OnInit, OnEvent, OnRender, shutdown;
    `GameApplication.h:187`), the whole of `DevStatsTile.cpp`. Dev keeps both doors exactly as before (`HE_GAME_DEV_HOTKEYS` from
    Debug/RelWithDebInfo or the option, OR hcfg bit 4096). A release tree warns if `-DHE_GAME_DEV_HOTKEYS=ON` is given.
  - `PROFILER_CAPTURE`: F9 handler, `Application::toggleProfilerCapture` (declaration and definition), `HE_PROFILE_*`, `HE_CAPTURE_FRAME/PATH`,
    `HE_PERF_INPUT_EVENTS`, `InputLatencyProbe` (`Application.cpp`).
  - `TEST_HOOKS`: `hiddenWindowRequested()` returns false and `backgroundFrameInterval()` the default (`Window.cpp`; the pure `*FromEnv`
    helpers stay), `HE_EXIT_AFTER_FRAMES` (`Application.cpp`), `HE_NO_SPLASH`/`HE_SPLASH_DUMP` (`SplashScreen.cpp`), `HE_CONFIG_FALLBACK_DIR` (`GlobalState.cpp`).
  - `ENV_OVERRIDES`: `HE_LOG`, `HE_LOG_NO_CONSOLE`, `HE_CONFIG_DIR` (`GlobalState.cpp`), `HE_ENGINE_CONTENT_EDITABLE` (`ContentManager.cpp`),
    `HE_PHYSICS_THREADS/_WATER/_BUDGET_MS` (`PhysicsTuning.h`, `PhysicsBudget.h`), `HE_PROPAGATE_FULL/_VERIFY` (`TransformHierarchy.cpp`).
  - The game logs `build profile '<dev|release>'` next to the runtime flavour. `BuildProfile.h` lists per switch what it covers.
  - `scripts/build_profile_strings.py --profile dev|release <dir>`: 33 items (env names as whole C strings, log text, `nm -C` symbols).
- **Verified:** both profiles `-fsyntax-only -Wundef -Wall` for the 9 changed TUs (clean). Dev tree (`out/build/release`, Release config):
  HorizonGame + he_tests build; `test_hidden_window` 9, `test_dev_stats_tile` 8, `test_build_profile` 5 (new case: he_tests gets the profile
  from HorizonCore, not the fallback), `test_physics_budget` 15, `test_physics_threading` 12, all green. Release tree
  (`out/build/release-profile`, `-DHE_BUILD_PROFILE=release -DHE_BUILD_TESTS=OFF`): HorizonGame builds, 733/733.
  Strings script: release tree 33/33 gone, dev tree 33/33 present; release mode against the dev tree failed on every item
  (negative control, run with the first 30-item list).
  By hand: `strings`/`nm -C` on libHorizonCore + HorizonGame agree. Dev smoke run (`HE_HIDDEN_WINDOW=1 HE_EXIT_AFTER_FRAMES=30`, deploy/Game):
  hidden, exits at frame 30, hotkeys OFF as before for a Release-config runtime, log says `build profile 'dev'`.
  Full ctest (`-j4`, private `HOME`, `HE_CONFIG_DIR` unset): 304/304 passed, the two `runtime_size_app_*` skipped (flavour-only).
- **Not verified:** the release HorizonGame was not launched (no hidden mode in it: it would open a visible window). MSVC/GCC not built
  locally (CI). Whether the editor, he_tests and the tools build in a release tree: they do NOT (expected, see traps).
- **Decisions taken:** feature switches, not the bare profile, guard each site (a `FEATURES` override must work, so no `HE_DEV_ONLY` where the
  guarded thing is a feature member); functions other libraries call stay and return the "off" answer, the F9 function itself is removed;
  class layouts are the same in both profiles (`SplashScreen::m_dumpIndex` stays). `HE_NET_*`, `HE_HCGEN_SDK`, `HE_PROFILING`, renderer env untouched.
- **Traps:** a release tree compiles only the runtime: `EditorApplication.cpp` calls `toggleProfilerCapture`, he_tests needs `DevStatsTile`.
  Build `--target HorizonGame` there and pass `-DHE_BUILD_TESTS=OFF`. A second tree needs `CMAKE_MAKE_PROGRAM` (CLion's ninja) and the
  `FETCHCONTENT_SOURCE_DIR_*` of the first; the configure line is in step 3's result post. The PUBLIC definitions changed every command line:
  the first build after this commit is a near-full rebuild (840 steps here).
- **Next steps:** step 4 (generated header). Step 5 must keep editor/tests out of a release tree (or make them build) and can call
  `build_profile_strings.py --profile release` on the SDK libs; step 9 runs it on an exported game.

## Handoff (step 4: compiled-in project configuration)

- **Topic / step:** Thema 211, step 4 of 9. Branch claude/export-sdk-compile-horizoncode-codegen, code 9320c26c (+ the `#if` fallback after it) and this doc.
  `CopilotDocs/HorizonGame.md:58,371` still describes the removed `HE_PROJECT_SECRET` as a design.
- **Done:** an exporter-side generator writes `HEGameBuildConfig.h`. It is one `constexpr HE::GameBuildConfig` aggregate with
  designated initializers, holding every `ProjectConfig` field (the A.1 hcfg row: names, flags, startup scene, save template, theme,
  bundle id, collision layers and audio buses as JSON) plus `Config/ProjectSettings.json` as JSON and the pak key XOR-masked
  (random mask per export). A game built with `-DHE_GAME_BUILD_CONFIG=<file>` reads neither file; without it the old path runs unchanged.
- **Files touched:** `src/HE_Core/include/Hpak/GameBuildConfig.h` (struct, generator, `readGameBuildConfigPakKey`, resolvers,
  `kGameBuildConfigFormat`, which the generated file `static_assert`s), `src/HE_Core/src/Hpak/GameBuildConfig.cpp`,
  `src/HE_Game/src/CompiledBuildConfig.{h,cpp}` (the only TU that includes the generated header; the define and include dir are
  set on that one source file), `src/HE_Game/CMakeLists.txt` (`HE_GAME_BUILD_CONFIG` cache path, checks the file name; removes the
  dead `HE_PROJECT_SECRET`), `GameApplication.cpp` (constructor peek, ProjectSettings, OnInit, key choice all go through the resolvers;
  `m_configSource` in `GameApplication.h`), `tests/test_game_build_config.cpp` + golden `tests/fixtures/game_build_config/HEGameBuildConfig.h`.
- **Precedence:** config = compiled > `project.hcfg` > none. Key = compiled > patched exe block > hcfg key (`HE::selectPakKey`).
  Compiled settings JSON `""` means defaults, and the file is not read. Logs name only the source ("project configuration compiled
  in (HEGameBuildConfig.h)", "pak key from ..."), never bytes.
- **Verified:** `he_tests --source-file='*test_game_build_config.cpp'` 5 cases / 208 assertions: generator output == golden text, the
  golden compiles in the test TU and reads back field by field, no 4-byte key run in the header, random masks differ but decode the same key,
  compiled wins over a different hcfg + settings file, compiled without any file, old path (hcfg + settings) and key order unchanged.
  Mutation (resolver ignores compiled values) turns 42 assertions red. `test_export_profiles` 449, `test_project_exporter` 324 and
  `test_project_settings` 262 assertions are green. Real binary (dev tree, `HE_HIDDEN_WINDOW=1 HE_EXIT_AFTER_FRAMES=30`, copies under `/tmp/s4_witness`):
  the prebuilt runtime next to an encrypted v2 hcfg logs "project configuration project.hcfg / pak key from project.hcfg". The same runtime
  built with the golden header logs "compiled in" for both, with that foreign hcfg beside it and also with no file at all. The plain key is in
  no 8-byte run of the executable, the masked bytes are there, and no log contains key bytes. Release tree (`out/build/release-profile`)
  builds HorizonGame with the header, and `build_profile_strings.py --profile release` reports OK. Both trees were reconfigured back to
  `HE_GAME_BUILD_CONFIG=` (empty) afterwards. Full ctest (`-j4`, private `HOME`, `HE_CONFIG_DIR` unset): 305 tests, all
  passed except the two `runtime_size_app_*` (skipped, flavour-only).
- **Not verified:** MSVC/GCC (CI). The forward declaration `struct ProjectConfig;` against the `HE_API` definition is untested on MSVC.
  No real export writes the header yet (step 8). Window title from the compiled settings was not checked on screen (hidden run).
- **Decisions taken:** one aggregate instead of loose constants (one place to keep in order, compile-time format check); JSON tails are
  dumped `ensure_ascii` into raw strings whose delimiter grows if the payload could close it; other strings use octal escapes
  (UTF-8-safe without `/utf-8`); the key is masked, not encrypted (same trust model as `EmbeddedPakKey.h`); `config.json` stays a player file
  (its defaults are not in the header, see "Pending").
- **Traps:** the golden fixture must be regenerated after a deliberate generator change:
  `HE_UPDATE_GAME_BUILD_CONFIG_FIXTURE=1 he_tests --source-file='*test_game_build_config.cpp'`, then rebuild (the TU includes the old file
  until it recompiles). JSON escapes `"`, so only a string ENDING in `)hecfg` exercises the delimiter growth. The generated header is as
  sensitive as the executable, so it must not land in the export folder or in git (the fixture's key is a test key).
- **Pending:** `config.json` defaults compiled in (topic goal "starts without a config file" still needs it, or an explicit decision
  that config.json may be absent, which the game already tolerates: absent key = member default). The exporter does not call the generator yet.
- **Next steps:** 1. Step 8: in the SDK path, build `ProjectConfig` as `writeProjectConfig` does (`ProjectExporter.cpp` phase 7; factor that
  into a function), call `HE::writeGameBuildConfigHeader` into the private build dir, configure with `-DHE_GAME_BUILD_CONFIG`, skip the
  key patch and the hcfg; incremental key reuse reads `HE::readGameBuildConfigPakKey` from the previous build dir. 2. Step 5: ship
  `Hpak/GameBuildConfig.h` among the SDK's public headers.

## Handoff (step 5: engine SDK)

- **Topic / step:** Thema 211, step 5 of 9. Branch claude/export-sdk-compile-horizoncode-codegen, code f42b15c5 (+ this doc).
  Also 4b0038d2: `.gitattributes` keeps `tests/fixtures/game_build_config/*` LF (step 4's Windows CI red: CRLF checkout of the golden header).
- **Done:** `cmake/HorizonSDK.cmake` (included at the end of the root `CMakeLists.txt:964`) installs the component `HorizonSDK`:
  `lib/` (HorizonCore/Net/Scene/Rendering + SDL3; Windows: DLLs in `bin/`, import libs in `lib/`), one flat `include/` (engine
  include + vendor dirs, glm, nlohmann, SDL3 + the generated `SDL_revision.h`, `HorizonVersion.h`), `share/HorizonEngine/game/`
  (the HorizonGame sources), `share/HorizonEngine/template/CMakeLists.txt` (from `cmake/sdk/template/`), `Shaders/` when the tree
  has Vulkan/D3D12 GI kernels, and `lib/cmake/HorizonEngine/` with `HorizonEngineConfig.cmake` (generated from
  `cmake/sdk/HorizonEngineConfig.cmake.in`), `…ConfigVersion.cmake` (ExactVersion) and `HorizonEngineGame.cmake` (`horizon_add_game`).
  Target `HorizonSDK` (not ALL) installs into `HE_SDK_DIR`, default `${DEPLOY_DIR}/SDK-<profile>` (never `<editor>/SDK`, which the
  editor's POST_BUILD wipes). `cmake --install <tree> --component HorizonSDK --prefix <dir>` does the same by hand.
- **Package:** imported targets `HorizonEngine::Core/Net/Scene/Rendering` + `SDL3::SDL3`, link chain as in the tree. Definitions
  per target = that library's `INTERFACE_COMPILE_DEFINITIONS` read at configure time (DLL macros, `HE_BUILD_PROFILE_*`, any
  `FEATURES` override; a generator expression there is a configure error), plus the directory-level `NOMINMAX`/`WIN32_LEAN_AND_MEAN`
  and `HE_PROFILING_ENABLED` on Core. File names come from `$<TARGET_FILE_NAME>`/`$<TARGET_LINKER_FILE_NAME>`. All paths relative
  to the config file. `find_package(HorizonEngine COMPONENTS dev|release)`: the other profile is "not found" with a reason.
  `horizon_add_game(<t> [OUTPUT_NAME] [BUILD_CONFIG <HEGameBuildConfig.h>] [SOURCES …])` mirrors `src/HE_Game/CMakeLists.txt`
  (cross-referenced in both); BUILD_CONFIG compiles a per-target copy of `CompiledBuildConfig.cpp`; Windows copies the DLLs beside the exe.
- **Test:** ctest `sdk_example_project` (`tests/CMakeLists.txt:1523`, driver `tests/sdk_example/run_sdk_example.cmake`): install,
  `file(RENAME)` the SDK, no tree path in the package files, required headers present; probe project (`tests/sdk_example/probe`, one
  exported call per library, prints profile + features, run); asking for the other profile must fail; the shipped template builds
  `SdkExampleGame` with the golden `HEGameBuildConfig.h`. Same generator/compilers as the tree, work dir `<build>/sdk-test`.
- **Verified (Mac, both trees):** dev tree `out/build/release`: test passes (29 s); the built game runs hidden
  (`HE_HIDDEN_WINDOW=1 HE_EXIT_AFTER_FRAMES=30`), dyld loads all five libs from the moved SDK's `lib/`, log says "project configuration
  compiled in", "pak key from HEGameBuildConfig.h", build profile 'dev', clean exit at frame 30. Release tree `out/build/release-profile`
  (HE_BUILD_TESTS=OFF, driver run by hand with `-DHE_PROFILE=release`): `--target HorizonSDK` OK, probe `profile=release features=0000`,
  `dev` refused, template game builds; `build_profile_strings.py --profile release <game> <sdk>/lib` OK (33/33 gone); dev SDK with
  `--profile dev` all present, with `--profile release` FAILED 33 (negative control). Full ctest (dev tree, `-j4`, private `HOME`,
  `HE_CONFIG_DIR` unset): 306/306, the two `runtime_size_app_*` skipped (flavour-only).
- **Not verified:** Windows/Linux (CI): DLL/import-lib names, `$ORIGIN` INSTALL_RPATH on the installed .so, the nested Ninja configure
  with cl.exe. The release game was not launched (no hidden mode in release). Tracy (`HE_ENABLE_TRACY`) SDKs: warning only, no Tracy headers.
- **Decisions taken:** hand-written imported targets, not `install(EXPORT)` (PUBLIC deps glm/SDL3/nlohmann/lua_static are in no export
  set); one SDK per tree and profile; game shipped as source (its HE_FEATURE_* sites and the compiled-in config are decided at game
  compile time), not as a static lib; `HE_PROFILING` propagated as built (forcing it off in release stays open); flat `include/` so
  `<sdk>` is also a valid `HE_HCGEN_SDK` root (not wired; `resolveSdk` untouched).
- **Traps:** macOS SDK libs still reference Homebrew lz4/zstd/openssl by absolute path; a shipped game needs `scripts/bundle_native_deps.sh`
  on its folder (step 8). The Python plugin is not in the SDK: the game looks for it beside libHorizonScene (`<sdk>/lib`) and runs Lua-only.
  The config text goes through `file(GENERATE)`: a literal `$<` in `HorizonEngineConfig.cmake.in` is evaluated; genex code belongs in
  `HorizonEngineGame.cmake`. `set(X "a" "b")` makes a list, so `NOT_FOUND_MESSAGE` is one string.
- **Next steps:** 1. Step 6: put the unified engine-API header under an installed include dir (any of the six in `HorizonSDK.cmake`
  is picked up) and add one call to the probe. 2. Step 8: build the release SDK (release tree, `--target HorizonSDK`), configure the
  template with `-DHorizonEngine_DIR=<sdk>/lib/cmake/HorizonEngine -DHE_GAME_BUILD_CONFIG=<private dir>/HEGameBuildConfig.h
  -DHE_GAME_NAME=<game>`, `horizon_add_game(... SOURCES <generated HorizonCode>)`, then copy runtime files from the SDK + bundle deps.
  Decide how the editor package carries a release SDK (second tree in CI packaging).

## Handoff (step 6: engine API header for generated code)

- **Topic / step:** Thema 211, step 6 of 9. Branch claude/export-sdk-compile-horizoncode-codegen, code e790638d (+ this sha line).
- **Done:** `src/HE_Scene/include/HorizonEngine/HorizonEngineApi.h` (`#include <HorizonEngine/HorizonEngineApi.h>`, installed with
  the SDK's flat include/). No second function list: it includes `EngineApi.h` (the functions, named by `ApiFn::cppCall`) and adds
  `HE::api::Scope` (the Ctx of one compiled instance + its creator push), `HE::api::pin::type<P>` / `toPin` / `toPinArray` (pin shapes,
  the thunks' result conversions). Engine events = the `CompiledInstance` hooks (header includes them).
  Registry: `enum class CppForm { Ctx, Free, Thunk }` + trailing `ApiFn::cppForm` (`EngineApi.h:3089,3113`), set in a fourth post-pass
  (`EngineApi.cpp:8679`: `kFreeGroups` 11 groups, `kFreeRows` 18, `kThunkRows` 10). `entityOrSelf`/`widgetOrSelf` (`EngineApi.cpp:8732,8752`)
  are now THE "0 means me" rule; the two selfDefault wrappers call them (same logs, same throttle site).
  Seam: `Context::apiCtx` (`HorizonCode.h:1297`, forward-declared `HE::api::Ctx`), `Services::apiCtx` (`HorizonCodeRuntime.h:536`),
  `Runtime::pushCreator/popCreator` (`:544`), bound in `HorizonCodeRuntime.cpp:1506`. Game (`GameApplication.cpp:1314`) and editor
  (`EditorApplication.cpp:2033`, one `callCtx` lambda now shared with callApi, sandbox + perm refresh included) fill it from the
  same builder as callApi.
  Emitter helpers `HorizonScene/EngineApiCpp.h` (`HE::api::cpp::valueType`, `callExpr`, `hookArgType`, `hookParams`,
  `directCallsSource`); HcCodegen's hook signature functions forward to them. `hc_codegen --api-direct FILE`.
- **Fixed on the way:** `engineEvents()` said `OnClipFinished` has an element (`HorizonCode.cpp:2816`), the hook and the fire site
  say no: a compiled class handling it would have emitted a non-compiling override. Only HcCodegen reads `elem`.
- **Tests:** `tests/CMakeLists.txt:1146` generates `engine_api_direct_gen.cpp` (637 direct calls, every non-Thunk row, + one override
  per engine event) INTO he_tests: a wrong cppCall/cppForm/params is a build error at the row's line (the first build failed exactly
  on OnClipFinished, nothing else). `test_engine_api_direct.cpp` (9 cases): table = non-Thunk rows in order, Thunk list pinned;
  426 rows run directly vs `invoke`, field-by-field Value compare, on a null Ctx and on two twin worlds in lockstep (two rounds,
  destroy rows last); negative control (desynced worlds must differ); `entityOrSelf`/`callExpr`; creator parity (`Scope` + direct
  `entity::spawnClass` names the same creator as `hc::callApi`); unbound Context. `test_engine_api_header.cpp`: the header as
  first include + `::hc` stays unambiguous. `sdk_example` probe includes the header from the installed SDK and calls a Free and a Ctx row (`api=1`).
- **Verified (Mac, dev tree out/build/release):** he_tests, HorizonGame, HorizonEditor build; test_engine_api_direct 9/9,
  _header 1/1, test_engine_api 130/130, test_horizoncode_codegen 53/53, hccompiled_loader, hc_bind_to, hc_pull_on_construct green;
  sdk_example_project passed. Full ctest (`-j4`, private `HOME`, `HE_CONFIG_DIR` unset):
  308/308, the two `runtime_size_app_*` skipped (flavour-only), 339 s.
- **Not verified / limits:** Windows/Linux only in CI. Not RUN (compiled only), asserted list `kNotRun`: anticheat, app, clipboard,
  cursor, datetime, db, dialog, fs, http, log, net, prefs, print, process, random, save, scene, settings, streaming, time, timer,
  window. The world pass reaches real state on 23 calls (entity, transform, camera, theme getters); the other rows compare their
  null-Ctx tolerance answers or, for the Free math/string/json rows, real computations. Physics/content/audio/widget rows ran
  without those services (null handles).
- **Decisions taken:** the header is a thin layer over EngineApi.h, not a copy; `cppForm` in the registry instead of a list in the
  emitter; Thunk rows stay on `hc::callApi` (no wrappers written); Scope takes the Ctx once (the interpreter takes it per call:
  same unless the host's world changes mid-body); the parity source is generated in C++ at build time (no Python in CI).
- **Traps:** do not `using namespace HE::api;` in generated code: `HE::api::hc` makes every `hc::` ambiguous. A bare
  `createEntity` has no TransformComponent: a world test on it compares defaults (the control test caught this). The design-time
  runtime (UIEditorPanel) binds no `apiCtx`: compiled code never runs there, but a direct call would bypass `designTimeAllows`.
- **Next steps:** 1. Step 7: emit `HE::api::cpp::callExpr(fn, "api", args)` for `cppForm != Thunk` with one `HE::api::Scope api(m_ctx);`
  per body, typed locals of `valueType(...)`, results via `pin::toPin`, multi-result rows as `auto [a_, b_] = …` (struct members are the
  results in order, proven by the generated file); keep `hc::callApi` for Thunk rows. 2. Link the generated library against
  HorizonScene (today only HorizonCore). 3. Optionally write direct wrappers for the 10 Thunk rows.

## Handoff (step 7: readable codegen with direct engine calls)

- **Topic / step:** Thema 211, step 7 of 9. Branch claude/export-sdk-compile-horizoncode-codegen, commits d509aa5b, 05f9111c,
  e576882d, 3a4f7099, 18b13b53 (link gap). First run was stopped by the queen; the resumed run closed the link gap.
- **Done (emitter, `src/HE_Scene/src/HcCodegen.cpp`):** Engine Call nodes on Ctx/Free rows are direct `HE::api::<group>::<fn>(api, …)`
  calls (`directRow`, `directCall`/`directRead`/`directStmt`), one `HE::api::Scope api(m_ctx);` per body that needs the Ctx, the API
  header included only by classes that call it. Thunk and Struct-pin rows keep `hc::callApi` (none left in the fixtures). Arguments
  are hoisted in pin order when two or more of them call something (`callsSomething`; value constructors do not count): C++ argument
  order is unspecified. Results go through `HE::api::pin::asInt/asRef/asColor/asVec4/asFloat` (`EngineApiCpp.cpp asPinHelper`) only
  where the C++ type can differ; multi-result rows are read by pin name (`resultMember`).
  Names: classes = asset stem in PascalCase (`AnimatorSync`; `classNameFor`, `reservedClassNames`), members/locals/parameters/results
  = authored names, public variables are public members (accessors gone), a body has its public method's name (same function
  without RunState, `Name(RunState& rs, …)` overload otherwise), other bodies `Tick`, `OnClicked_element3`, `Jump_Pressed`,
  `afterDelay`; node state `doOnceDone`/`flipFlopIsA`; RunState fields `<node>_<pin>` (`rs.sum_total`); temporaries via `temp()`
  (unique per body: `items`, `target`, `args`). Base-chain names are claimed (`CompiledClass::names`), overrides keep theirs.
  `hcgen_<Class>.cpp` = logic, `hcgen_<Class>.reflect.cpp` = identity, slots, dispatchers, factory.
- **Removed (what step 1, B.2/B.3, called in the way):** the `f_`/`ev_<id>` body + forwarding-wrapper split, the per-call
  `std::vector<hc::Value>` boxing + string dispatch for direct rows, the `r<id>` result vector nobody read, the accessors.
  Kept on purpose: `fireEvent`/hooks (the Runtime fires by name AND by hook), `slots()`/`callFunction` (savegames, Bind To, RPC).
- **Tests:** `test_hc_codegen_readable.cpp` (golden files `tests/fixtures/hcodegen_golden/`, LF via .gitattributes; update with
  `HE_UPDATE_HCGEN_GOLDEN=1`; old cryptic forms banned; every callApi has no direct form). `directCallsSource` static_asserts the
  "used as returned" result types. Parity harness: `checkParity` drops direct rows' `callApi` trace lines on both sides (lesson 452).
- **Link (18b13b53):** `generateCMakeLists` uses the engine SDK package (`HorizonEngine::Scene`) when `HorizonEngine_DIR` or
  `HE_SDK_DIR/lib/cmake/HorizonEngine` exists, else the explicit dirs, linking `HorizonScene HorizonCore` (`HE_SDK_LIB_DIR` may be a
  list). `SdkInfo` + `extraLibDirs` (JSON `libDirs`) + `packageDir`; `sdkFromRoot` adds `src/HE_Scene/include`; `buildDylib` passes
  both. The editor stages `src/HE_Scene/include` and HorizonScene's linker file into `<editor>/SDK`; `he_sdk_config.json` lists them.
  `hc_codegen --cmake` writes the export's CMakeLists. `test_hc_codegen_export_build.cpp`: dev `he_sdk_config.json` → `resolveSdk`
  → `generate` (engine_world + animator_sync) → `buildDylib` → `CompiledClassTable` → engine_world against the interpreter on twin
  real worlds (owner entity via `setOwnedEntity`, one Ctx builder for callApi and apiCtx): variables and transforms equal.
- **Verified (Mac, out/build/release, dev):** HorizonEditor + all targets build; full ctest 310/310 (2 flavour skips, private HOME,
  `HE_CONFIG_DIR` unset). By hand: all 50 fixture classes built through `hc_codegen --cmake` against ONLY the staged
  `out/deploy/Editor/SDK` (include dirs + lib); the same without HE_Scene fails ("HorizonEngineApi.h not found").
- **Not verified:** Windows/Linux (CI, see the run in the result post); the package branch of `generateCMakeLists`
  (`HorizonEngine_DIR`) was not built — no test installs the SDK and builds generated code against it (the export in step 8 will).
  `entity.spawn` makes a bare entity, so the fixture's setPosition on it is a no-op on both sides (the test compares that, too).
- **Next steps (step 8):** point the export at the installed SDK (`HE_HCGEN_SDK` = `SDK-<profile>` → `packageDir`) and build the
  game with the generated classes; extend `tests/sdk_example/run_sdk_example.cmake` to build `hc_codegen --fixtures --cmake`
  output against the installed SDK. Optional: wrappers for the 10 Thunk rows.

## Handoff (step 8: export compiles the game from the SDK)

- **Topic / step:** Thema 211, step 8 of 9. Branch claude/export-sdk-compile-horizoncode-codegen, commits 1895eb22, e3b0401f,
  759621e3, 20312925, 49393404, f2d08ace (+ this doc). CI dispatched: https://github.com/Horizon-Creations/HorizonEngine/actions/runs/39159490500 (not seen).
- **Done:** `src/HE_Editor/ExportSdkBuild.{h,cpp}` (ImGui-free): `findGameSdk` (HE_GAME_SDK_DIR as given, else `<editor>/SDK-<profile>`,
  else `<editor>/../SDK-<profile>` = DEPLOY_DIR, where `--target HorizonSDK` installs), `unavailableReason`, `writeBuildConfigFromExport`
  (exported `project.hcfg` + `Config/ProjectSettings.json`; key from the hcfg or, when the exporter patched it into the prebuilt exe,
  `readEmbeddedPakKey` on that exe), `makeBuildSpec` (SDK template, `HorizonEngine_DIR`, `HE_GAME_BUILD_CONFIG`, private work dir),
  `installBuiltGame` (exe + SDK `lib/*.dylib|*.so*`, `bin/*.dll` over the prebuilt copies), `relinkToBundledDeps` (macOS: the SDK
  libs' `/opt/homebrew`/`/usr/local` links become `@rpath/<name>` when the folder bundles that lib, then ad hoc re-sign; mirrors
  `scripts/bundle_native_deps.sh:84-107`). Dialog `ExportDialogPanel.cpp:1175`
  checkbox "Compile game from SDK" with the SDK path or "Will use the prebuilt game: <reason>"; worker step "Compile game"
  (`:1598`, after Clean up) calls `compileGameFromSdk` (`:555`) only after a successful export, so every failure leaves the
  prebuilt export as it was. Profile = Dev Tools ? dev : release. Work dir `$TMPDIR/HorizonSdkGame/<project>-<profile>` (header holds
  the key, never in the export folder; kept for incremental builds). macOS `.app`: ad hoc `codesign --force --deep` after the swap.
  Cancel: `DylibBuildSpec::cancel` (`HcCodegen.h`), checked per line in `runStreaming` (`HcCodegen.cpp:4611`), outcome "cancelled";
  `Build::setCancelable/cancelFlag` + Cancel button in the build window (`BuildProgressDialog.cpp:945`). Help entries
  `Export/Compile game from SDK`, `Build Window/Cancel`; `editor_help_audit.py --check` OK. PATH: `buildDylib`→`resolveCmake` already
  runs `ensureToolPathAugmented()`.
- **Verified (Mac, dev tree `out/build/release`):** `he_tests --source-file='*test_export_sdk_build.cpp'` 5 cases green (lookup order,
  profile must match, reasons, header + key round trip, refusal without key, spec defines, swap). E2E (gated):
  `HE_EXPORT_SDK_E2E=out/deploy/SDK-dev HE_EXPORT_SDK_E2E_EXPORT=<export copy>`: real cmake build of the SDK template (~45 s), cancel
  gives "cancelled", swap over an export folder with today's prebuilt runtime, the game started hidden runs 30 frames, exit 0, logs
  "project configuration compiled in (HEGameBuildConfig.h)" + "pak key from HEGameBuildConfig.h", and `otool -L` of every swapped
  file has no `/opt/homebrew/` (56 assertions). The SDK-built exe's rpath lists `@executable_path` first (checked on SdkExampleGame),
  so the folder's libs load. Editor + he_tests build; related
  files green (project_exporter 34, export_profiles 50, horizoncode_codegen 53, hc_codegen_readable 4, game_build_config 5,
  export_backend 11, hc_codegen_export_build 1). Full ctest (`-j4`, private HOME, HE_CONFIG_DIR unset, before f2d08ace): 310/311,
  `runtime_size_app_*` skipped; `test_collab_controller` failed under -j4 and passed alone (42 s), so I take it for a flake.
- **Not verified:** a click-through export in the GUI (no live editor run); the release SDK path (no `SDK-release` built here);
  `.app` bundle swap + codesign; Windows/Linux (CI); the Cancel button on screen.
- **Decisions taken:** compile AFTER the prebuilt export and swap (fallback is free, the exporter is unchanged); header from what
  the export wrote (one source of truth: `writeProjectConfig`), not a second ProjectConfig builder; strict profile match (no dev SDK
  for a Shipping export); `project.hcfg` stays in the folder (compiled config wins; incremental key reuse reads it); checkbox is
  session-only (not in the .heproj profile yet).
- **Not done (against the step text "generierten HorizonCode-Code einbeziehen"):** generated HorizonCode is NOT built into the SDK
  game. It still ships as the separate `libHorizonCodeGen`, built against the editor's dev codegen SDK, so a release-SDK game
  plus that dylib is a dev/release profile mix that nobody has loaded yet.
- **Traps:** swapping SDK engine libs into a folder whose `libHorizonPython.dylib` is from an OLDER engine build segfaults after
  PlayerHost (seen with step 4's 08:50 witness); the SDK has no Python plugin, so SDK and prebuilt runtime must come from the same
  build. The HorizonCode dylib is built against the editor's dev codegen SDK, so with a release game it is a profile mix (untested).
  macOS SDK libs still reference Homebrew lz4/zstd/openssl by absolute path (step 5 trap): the prebuilt folder's bundled copies
  were not what the SDK libs loaded (fixed by `relinkToBundledDeps`; a dep the folder does NOT bundle stays absolute). The SDK
  game keeps a second, absolute rpath to `<sdk>/lib` (harmless, leaks a path). `findGameSdk` runs per frame in the dialog (a few stats).
- **Next steps:** 1. Step 9: GUI export with the checkbox on (dev and a built `SDK-release`), start the game, `build_profile_strings.py
  --profile release` on the swapped exe. 2. Ship `SDK-<profile>` in the editor package (CI packaging) and copy the Python plugin from
  the SDK tree or fail the swap when the folder holds one of another build. 3. Persist the checkbox in the export profile (.heproj).
  4. Build the generated HorizonCode against the same SDK as the game (its `packageDir` branch in `generateCMakeLists`), or link the
  sources into the exe (`horizon_add_game SOURCES` + a static registration hook). 5. Strip the absolute rpath (`install_name_tool -delete_rpath`).

## Handoff (step 9: one SDK, headers + lib sets only)

- **Topic / step:** Thema 211, step 9 of 12. Branch claude/export-sdk-compile-horizoncode-codegen, commits ae5c9e6f (cmake), 0a57ad76
  (editor) + this doc, pushed. Follows the human's posts 1518/1519 (headers + libs only, no repo/sources, one SDK). The board title
  still says "Quell-SDK: gesamter Engine-Code", which post 1519 replaced. Open question #35 (how dev code stays out of a release game
  with ONE SDK) was not answered, so the queen's interim option 2 is built: one package, a lib set per profile.
- **Layout** (`cmake/HorizonSDK.cmake`, header comment): `include/` (shared), `lib/<profile>/` (Windows: `bin/<profile>/` DLLs),
  `lib/cmake/HorizonEngine/` = neutral `HorizonEngineConfig.cmake` (plain `configure_file`, no genex) + `HorizonEngineTargets-<profile>.cmake`
  (from `cmake/sdk/HorizonEngineTargets.cmake.in`, `file(GENERATE)`, imported targets + that set's definitions) + `HorizonEngineGame.cmake`
  + header stamps, `share/HorizonEngine/{game,template}`. Each tree installs its own set: `--target HorizonSDK` into
  `HE_ENGINE_SDK_DIR` (renamed from `HE_SDK_DIR` so old caches don't keep installing into `SDK-<profile>`; default `${DEPLOY_DIR}/EngineSDK`).
- **Profile choice** (`cmake/sdk/HorizonEngineConfig.cmake.in`): `COMPONENTS dev|release`, else the consumer's `HE_BUILD_PROFILE`, else
  the only installed set; none/two-without-choice/missing set = "not found" with the reason. Header stamp: every install hashes
  `include/` (without the generated `SDL3/SDL_revision.h`, which differs per tree's SDL fetch) into `HorizonEngineHeaders.txt` and
  `HorizonEngineHeaders-<profile>.txt`; a set whose stamp differs is refused (prebuilt fallback instead of a crash on an ABI mix).
- **Pruned** (no compiler input; a grep of every public header + `src/HE_Game/src` found only comments naming them): `HE_Core/vendor`
  (stb, fonts, bc7enc), `miniaudio.h`, loose `entt.hpp`/`fwd.hpp`, SDL `SDL_test*`/`SDL_opengl*`/`SDL_egl.h`, `share/HorizonEngine/Shaders`
  (runtime data; the prebuilt export folder has it), SDL namelink. Size (macOS, Release): before `SDK-dev` 47 MB (include 19, lib 28,
  share 0.25), so two SDKs would be 94 MB; after one set 39 MB, both sets 68 MB (include 11, lib/dev 28, lib/release 28). `strip -x`
  would take a set from 28 to 23 MB; not done (arm64 code signature, and the stripped libs ship in the game).
- **Editor** (`src/HE_Editor/ExportSdkBuild.{h,cpp}`): `kGameSdkDirName = "EngineSDK"`; `findGameSdk` needs the profile's Targets file;
  `makeBuildSpec` adds `HE_BUILD_PROFILE=<profile>`; `installBuiltGame` copies `lib/<profile>`, `bin/<profile>`. Template takes
  `-DHE_BUILD_PROFILE`. Help text `Export/Compile game from SDK` updated (audit OK).
- **Verified (Mac):** driver by hand with both trees (`-DHE_BUILD_DIR_2=out/build/release-profile -DHE_PROFILE_2=release`, 35 s): both
  probes run (`profile=dev features=1111`, `profile=release features=0000`), refusals (no choice with two sets, stale stamp), template
  game built per set; `build_profile_strings.py --profile release` on the release game + `lib/release`: OK 33/33 gone, dev set fails it
  (negative control). ctest `sdk_example_project` (one set: default choice, missing set refused, stale stamp) green. Both trees wrote
  the same stamp into `out/deploy/EngineSDK`. `he_tests --source-file='*test_export_sdk_build.cpp'` 6/6; e2e against that SDK:
  dev with `HE_EXPORT_SDK_E2E_EXPORT` (game ran 30 frames, "project configuration compiled in", profile 'dev'), release (build + swap).
  Full ctest (dev tree `out/build/release`, `-j4`, private HOME, HE_CONFIG_DIR unset): 311/311, `runtime_size_app_*` skipped.
- **Not verified:** Windows/Linux (bin/<profile> DLL copy, `$ORIGIN` in lib/<profile>, SDL soname without namelink): CI only. Release game
  not started (no hidden mode). GUI click-through.
- **Decisions taken:** one package with lib sets (option 2, cheap to change: only the Targets/lib dirs are per profile); refusal, not
  warning, on a stamp mismatch; old `SDK-<profile>` folders are no longer found (no release shipped them).
- **Traps:** the two trees have different `DEPLOY_DIR`s (`out/deploy` vs `out/deploy-release-profile`): pass the same
  `-DHE_ENGINE_SDK_DIR` to both. Messages through the config's `_he_not_found` macro lose every `;` (macro ARGN). A `cmake --install`
  uses the last configure's `cmake_install.cmake`: re-run `cmake <tree>` after editing `HorizonSDK.cmake`.
- **Next steps:** 1. Step 10: the editor builds from `EngineSDK` with the chosen profile and caches by SDK stamp + profile + settings
  (`HorizonEngineHeaders.txt` is a ready key part). 2. Step 11: `generateCMakeLists`' package branch (`HcCodegen.cpp:4374`) calls
  `find_package(HorizonEngine)` without a profile; with two sets that is refused (interpreted fallback), so pass `HE_BUILD_PROFILE`
  (and `sdkFromRoot`'s `libDir = root/lib` has no libs any more in this layout). 3. Ship `EngineSDK` (both sets) in the editor package (CI).

## Handoff (step 10: one HorizonEngine library, export layout, profile check)

- **Topic / step:** Thema 211, step 10 of 13. Branch claude/export-sdk-compile-horizoncode-codegen: 0f30c102 (bundle, game layout,
  profile check), 7cd0e989 (exporter layout, SDK lib set), d1759278 (plugin rule). Follows the human's posts 1521/1522.
- **Done:** `HorizonEngine` (`cmake/HorizonEngineBundle.cmake`, included in the root after HE_Rendering) is ONE shared library linked
  from `$<TARGET_OBJECTS>` of Core/Net/Scene/Rendering and of every in-tree static lib that links one of them (renderer backends,
  he_materialshader, he_shadercompiler), so no part library is ever on its link line (no second compile, one link), plus
  `HorizonPythonEngine` (the plugin's objects against it). Editor, tests, tools keep the four libs. `HorizonGame` links the bundle; deploy `out/deploy/Game` = exe + Python runtime
  (zip, `_pth`, lib-dynload) + `HorizonEngine/` (engine, SDL3, plugin, bundled lz4/zstd/crypto/libpython, Shaders/). Export of such a
  runtime = `<ProjectName>(.exe)` + `data/` + `HorizonEngine/` (`ProjectExporter.cpp` prepareOutputLayout/copyRuntimeBinaries,
  `exportedExecutableName`, `runtimeHasEngineDir`); a runtime without `HorizonEngine/` keeps the flat layout and `HorizonGame`. Shared
  names + `dataDir()` in `Platform/GameLayout.h`; game data reads go through it. `HorizonEngine_BuildProfile()` (`Platform/EngineProfile.h`,
  extern "C", in Core so in the bundle too); `HE_Game/src/main.cpp` refuses a library of another profile (stderr + message box, exit 3).
  SDK `lib/<profile>` = libHorizonEngine + SDL3 + plugin (set wiped on install); `HorizonEngine::HorizonEngine`, `::Core/::Net/::Scene/
  ::Rendering` are INTERFACE forwards. `installBuiltGame` swaps into `HorizonEngine/`. User-visible: mod overlays now live in
  `data/Mods/` (the game's data dir), no longer beside the executable.
- **Windows (written, NOT run):** the static libs above are absorbed because otherwise MSVC resolves Scene's `__imp_` refs to Core
  against HorizonCore.lib (transitively linked) and the DLL imports HorizonCore.dll: two engines. Check on Windows:
  `dumpbin /dependents HorizonEngine.dll` must not list HorizonCore/Scene/Net/Rendering. Engine + SDL3 are `/DELAYLOAD`ed, `HE_Game/src/EngineDllDir.cpp`'s `__pfnDliNotifyHook2` loads them
  from `<exe>\HorizonEngine\` (LOAD_WITH_ALTERED_SEARCH_PATH). Risk: LNK1194 if the exe imports engine DATA; LNK4217 warnings in the
  bundle link (Scene/Net/Rendering objects dllimport Core). Export-all on the bundle: ~13k symbols on macOS, far below 65535.
- **Verified (Mac, dev tree out/build/release + release tree out/build/release-profile):** full ctest on the step 9 state 311/311
  (posted, 1524). Bundle links with no part library (`otool -L`); `stbtt` collided (Core UIFont.cpp + Scene UISystem.cpp), Scene's copy
  now `STBTT_STATIC`. Deployed game 30 frames hidden, all dylibs from `HorizonEngine/` (DYLD_PRINT_LIBRARIES). Mismatch: dev exe +
  release lib -> dyld refuses before main (`DevStatsTile::toggle` missing, 134); release exe + dev lib -> main's check message.
  Exporter tests (new layout, flat fallback, names, Python filter) + `installBuiltGame` into `HorizonEngine/`; `test_app_todo` exports
  `TodoApp` + `data/` + `HorizonEngine/` and boots it 30 frames. Two-set SDK driver (`-DHE_BUILD_DIR_2=out/build/release-profile
  -DHE_PROFILE_2=release`): probes print `library profile=dev|release`, refusals, template per set. `build_profile_strings.py --profile
  release` on SDK lib/release and the release Game folder: 33/33 gone; dev set fails it. SDK e2e on the todo export (`HE_EXPORT_SDK_E2E_EXE
  =TodoApp`) dev + release: release game ran with 'release' profile, config compiled in, pak from data/. Plugin variant dlopens from
  `HorizonEngine/` with libpython beside it. Full ctest at the end: see the result post.
- **Not verified:** Windows/Linux start (CI compiles only). A Python project running scripts in the new layout. .app export with the
  bundle. GUI click-through. The message box path's exit code 3 (needs a click).
- **Known regression:** compiled HorizonCode (`libHorizonCodeGen`, links the separate Scene/Core, rpath `@loader_path`) does not load in
  the new layout -> the game logs it and runs INTERPRETED. Step 12 builds it against the bundle.
- **Decisions taken:** objects bundle, not a re-compile; delay-load hook instead of an SxS private assembly (probe order with a DLL named
  like its folder unverifiable here); Python interpreter files stay beside the exe on Windows/Linux (stdlib bootstrap), libpython goes
  into `HorizonEngine/` on macOS (bundled as the plugin's dependency); Shaders in `HorizonEngine/Shaders` (backends try it before
  `<base>/Shaders`), in a .app in Resources/Shaders; layout follows the runtime, so old cross-platform bundles still export.
- **Traps:** imported targets found in a subdirectory (OpenGL::GL, Vulkan::Vulkan, CURL::libcurl) are re-found at root scope by the
  bundle file; a missing one fails the configure (a failed generate leaves a STALE build.ninja behind, check `Generate step failed`).
  Release games ignore HE_HIDDEN_WINDOW/HE_EXIT_AFTER_FRAMES (lesson 458): never start one without a kill deadline. The SDK
  e2e test now runs the game only for dev. `-dead_strip_dylibs`/`--as-needed` on the bundle are what keep libHorizonCore off its deps
  (the backend static libs link HorizonCore). bundle_native_deps.sh takes `<engine dir> <exe dir>` and removes stale top-level copies.
- **Next steps:** 1. Step 11: export builds from `EngineSDK` with profile + cache (key: `HorizonEngineHeaders.txt` + profile + settings);
  `makeBuildSpec` still names the exe HorizonGame, the swap renames. 2. Step 12: HorizonCodeGen against `HorizonEngine::HorizonEngine`
  (`HcCodegen.cpp` generateCMakeLists ~4374 uses `HorizonEngine::Scene`, now a forward; pass HE_BUILD_PROFILE), shipped into
  `HorizonEngine/` (game loads it from `data/` today, `GameApplication.cpp` compiledClasses().load). 3. Windows run of an export on NN-WS03.

## Handoff (step 11: export builds from the SDK with profile and cache)

- **Topic / step:** Thema 211, step 11 of 13. Branch claude/export-sdk-compile-horizoncode-codegen: 59c4b01d (cache, profile field,
  PORTABLE rpath, MCP, help, tests), dfad4bd0 (OBJECT_DEPENDS fix) + this doc.
- **Profile:** `ExportSdk::sdkProfileFor(devTools)` (`ExportSdkBuild.h`): devTools on = `dev` lib set, off = `release`. With the
  seeded profiles that is exactly Development->dev, Shipping->release (`profileFromJson` defaults devTools from the name), so no
  name-based mapping was added. Only the game is compiled (template: main, GameApplication, EmbeddedPakKey, CompiledBuildConfig +
  the header), against `lib/<profile>` with that set's definitions (Targets-<profile>.cmake).
- **Cache** (`ExportSdkBuild.cpp:141-266`, used at `ExportDialogPanel.cpp:557-640`): work dir per project + profile in
  `GlobalState::userDataDir()/SdkGameBuilds/<project>-<profile>` (beside EngineContentCache; TMPDIR only when there is no user dir;
  macOS purges TMPDIR, so the old `$TMPDIR/HorizonSdkGame` was no cache). `cache-key.txt` = one line per input: sdk root, package
  version, header stamp of the set, SHA-256 over name/size/mtime of the set's files, profile, toolchain (`cmake <ver>, <compilerId>`
  from the editor's ToolchainProbe, "unknown" without one), every -D. Miss -> the log names the first differing input, the build
  tree is removed, key written. Hit -> tree reused, only what changed builds. Failed build -> key forgotten; cancel keeps it. Export
  settings reach the binary only via the defines and the header, whose content the build tracks (see the fix below).
- **Rpath:** `horizon_add_game(... PORTABLE)` / template `-DHE_GAME_PORTABLE=ON` = `BUILD_WITH_INSTALL_RPATH ON`; `makeBuildSpec`
  passes it, so the game carries only `@executable_path/HorizonEngine;@executable_path` (`$ORIGIN` on Linux). Opt-in: the template
  run from its own build dir (sdk_example) still needs the SDK rpath.
- **Checkbox persisted:** `ExportProfile::compileGameFromSdk` (`ProjectManager.h`), json key `compileGameFromSdk` (default false),
  dialog load/save, MCP `project_export` argument + result, profile listing in McpToolsSettings. Help text extended (audit 1206/1206).
- **Bug found and fixed (dfad4bd0):** a rewritten `HEGameBuildConfig.h` did NOT recompile anything in a kept tree (per-source include
  dir, invisible to Make's dependency scan) -> the old config/key stayed compiled in. Since step 8 the work dir was kept, so this was
  live before the cache. `OBJECT_DEPENDS` in `cmake/sdk/HorizonEngineGame.cmake:61` and `src/HE_Game/CMakeLists.txt:123`. Lesson 459.
  The cause is inferred from the symptom (not confirmed in a kept tree's depend files); the fix is proven by the e2e either way.
- **Verified (Mac, dev tree out/build/release, release tree out/build/release-profile, SDK out/deploy/EngineSDK, both sets stamp
  59151f..):** `test_export_sdk_build` 10 cases / 86 assertions (key per input, miss names toolchain/headers/libs, hit keeps tree,
  forget); `test_export_profiles` 50/50 (round trip); `test_mcp_tools_build` green. Gated e2e: dev and release each: first build miss,
  rewrite header, second build hit 3.6-3.9 s (vs ~35 s) and its build.log compiles `CompiledBuildConfig.cpp` and not
  `GameApplication.cpp`; `otool -l` has no absolute LC_RPATH. Over a real TodoApp export (`HE_EXPORT_SDK_E2E_EXPORT=<copy>
  HE_EXPORT_SDK_E2E_EXE=TodoApp`, dev): swap, game 30 frames hidden, "project configuration compiled in", profile 'dev', no Homebrew
  links (23/23). HorizonEditor + all targets build. Full ctest (-j4, private HOME, HE_CONFIG_DIR unset): 311/311, rc 0, runtime_size_app_* skipped.
- **Not verified:** a GUI click-through (dialog progress lines "Build cache hit/miss" are only seen in code + e2e equivalents); the MCP
  path live; Windows/Linux (CI compiles only; `BUILD_WITH_INSTALL_RPATH` on Linux links against libHorizonEngine.so whose SDL3 dep
  must be found at link time, SDL3 is on the link line directly). Release game not started (lesson 458).
- **Traps:** the SDK install in out/deploy was older than HEAD (1d53f47f changed ProjectExporter.h after it): reinstall BOTH sets
  (`--target HorizonSDK` in both trees) or the package refuses the stale set. ctest/he_tests from the build dir only.
  The cache has no eviction: one tree per project + profile stays in `SdkGameBuilds/` (with the key-bearing header), by design.
- **Next steps:** 1. Step 12: HorizonCodeGen against `HorizonEngine::HorizonEngine` + `HE_BUILD_PROFILE`, into `HorizonEngine/`;
  give it the same cache key/work-dir helpers (`buildCacheKey` takes any DylibBuildSpec). 2. Step 13: GUI export twice per profile,
  second one logs "Build cache hit"; `build_profile_strings.py --profile release` on the swapped Shipping game.

## Handoff (step 12: CI red, Windows LNK1189 and Linux runtime_size)

- **Topic / step:** Thema 211, step 12. Commits d27a46dd (Windows), 50a23711 (Linux), e2daabd6 (Editor/Game), pushed.
  CI https://github.com/Horizon-Creations/HorizonEngine/actions/runs/39172175638 and Runtime flavours 39172176022 on e2daabd6,
  not seen finished.
- **Windows LNK1189** (CI 39168157588, 39170780791, Runtime flavours 39170502233, the only red there): the 65535 counts the members
  of the IMPORT library, one per exported symbol, not input objects. `WINDOWS_EXPORT_ALL_SYMBOLS` on the bundle exported every
  external symbol of every object (four parts + absorbed backends/shader compilers, inline and template instantiations included).
  Only HorizonScene uses export-all; Core/Net/Rendering export via `HE_*_BUILD_DLL` dllexport, carried in their objects. Fix
  (`cmake/HorizonEngineBundle.cmake`, WIN32 block): export-all OFF, `HorizonEngine.def` from `$<TARGET_OBJECTS:HorizonScene>` via
  `cmake/HorizonEngineBundleDef.cmake` (`cmake -E __create_def`, undocumented, the same command CMake runs for export-all). The
  build log prints "HorizonEngine.def: N symbols ..." and fails over 65535. Static archives instead of objects were rejected:
  export-all never walks archive members, and unreferenced Scene objects would be dropped. The editor's separate libs are unchanged.
- **Linux runtime_size** (310/311 in 39168157588): not the bundle. libHorizonEngine.so 31.5 MB vs the four 32.5 MB (17.10 run
  39031060917). New was crypto 5.3 MB: `bundle_linux` ran `ldd` (whole closure) on libHorizonEngine.so, which reaches the system
  libcurl's libssl/libcrypto through HorizonNet; libHorizonCore.so, the old reference, never did. Now only direct NEEDED
  (readelf -d, objdump -p, closure as fallback) of the wanted sonames are copied. Expected: about 37.0 MB without python, threshold
  unchanged at 42. `runtime_size.py`: bucket `engine` (HorizonEngine), shaders also under `HorizonEngine/Shaders/`.
- **Found in passing:** `Editor/Game` (editor POST_BUILD `copy_directory`, which never deletes) kept the four flat libs from before
  step 10 in an upgraded tree, so exports shipped them. It is now emptied before the copy (`src/HE_Editor/CMakeLists.txt:611`).
- **Verified (Mac, out/build/release, dev):** the .def mechanics in a throwaway CMake project (file(GENERATE) of TARGET_OBJECTS,
  genex DEPENDS, the script, count line, null rebuild); NEEDED parsing on canned readelf/objdump output + `bash -n`; build of all
  targets rc 0; `Editor/Game` holds only HorizonEngine/{libHorizonEngine,libHorizonPython}; runtime_size game 34.6 MB without python
  (engine 25.8), OK. **Full ctest on e2daabd6 in the foreground** (-j4, private HOME, HE_CONFIG_DIR unset): 311 tests, 309 passed,
  0 failed, 2 skipped (runtime_size_app_*), rc 0, 343 s. (Earlier "311/311" counted these two skips as passes.)
- **CI only:** that HorizonEngine.dll links and the total (def + dllexport) stays under 65535, that the game and the Python plugin
  still resolve every Scene symbol on Windows, Linux without libcrypto at about 37 MB. The Windows code path never ran on the Mac.
- **Risk:** if the Scene def count + Core/Net/Rendering dllexports still pass 65535, the next step is filtering COMDAT
  inline/template symbols out of the def (consumers instantiate their own); the logged count tells how close it is.
- **Next steps:** 1. Read the "HorizonEngine.def: N symbols" line and runtime_size in 39172175638. 2. Step 13 (HorizonCodeGen against
  the bundle).
