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
