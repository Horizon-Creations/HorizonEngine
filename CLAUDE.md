# Horizon Engine — notes for every Claude instance working here

C++20 engine + ImGui editor (macOS, Windows, Linux; five render backends). Read this
file, then **only** what your task needs. Every worker that re-reads the repo from
scratch costs tokens again; the sections below exist so you do not have to.

## 1. Where things are

| Target / dir | What lives there |
|---|---|
| `src/HE_Core` (HorizonCore) | Application/Window/Input, `ContentManager` + `.hasset`/`.hpak` (Hpak/), UIWidget (in-game UI), HorizonCode (visual scripting), MaterialGraph, ParticleGraph, Physics (Jolt), Audio, Scripting, JobSystem, Project |
| `src/HE_Scene` (HorizonScene) | `HorizonWorld` (entt ECS), `Components/`, all runtime systems (animation, physics sync, weather, foliage, terrain...), `SceneSerializer`, `EngineApi.cpp` (script API) |
| `src/HE_Rendering` | `HorizonRendering` + backends under `src/Backends/{Metal,OpenGL,Vulkan,D3D11,D3D12,Software}`; sky/atmosphere shader source in `include/HorizonRendering/SkyShaderSource.h` (Metal has a hand-kept mirror in `MetalRenderer.mm`) |
| `src/HE_Editor` | The editor (`HorizonEditor`): one `*Panel.cpp` per panel, `EditorUI.cpp` (menus, panel order), `EditorApplication.cpp` (app state), `EditorHelp.cpp` (tooltip/F1 text), `EditorWidgets.*`, `EditorTheme.*`, MCP bridge (`Mcp*.cpp`) |
| `src/HE_Tools` | `HorizonImporters` (`AssetImporter/`: glTF native, FBX/OBJ/DAE/BLEND via Assimp), `asset_compiler`, `hpak_packer`, `hc_codegen`, `*_gen` content generators |
| `src/HE_Game` | `HorizonGame`, the player executable |
| `src/HE_Net` | sockets, collaboration transport |
| `src/HE_SourceControl` | git/LFS CLI wrapper + `GitService` worker (used by `SourceControlPanel`, `GitController`) |
| `src/HE_ContentSync` | EngineContent SFTP sync/publish (credentials compiled in, see its CMakeLists) |
| `src/HE_Python` | Python scripting host |
| `tests/` | one doctest binary `he_tests`; every test file is listed in `tests/CMakeLists.txt` |
| `docs/` | plans, audits, design records. `docs/coding-conventions.md` is the decision record for naming/namespaces |

Entry points: editor `src/HE_Editor/main.cpp`; game `src/HE_Game/src/`.

## 2. Build and test

```
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target HorizonEditor -j 8     # or he_tests, asset_compiler, HorizonGame
cmake --build cmake-build-release --target he_tests -j 8
ctest --test-dir cmake-build-release -R test_outliner_ui --output-on-failure   # one test file (CTest name = file stem)
# or run the he_tests binary directly: he_tests --source-file=*test_outliner_ui.cpp
python3 scripts/editor_help_audit.py --check                                # every editor control needs a help entry
```

* Assimp (`HE_ENABLE_ASSIMP`), SFTP credentials and so on are configure-time options; a build
  without them is normal and supported.
* In a container without the full toolchain, syntax-check a file instead of building
  everything: `g++ -std=c++20 -fsyntax-only -DHE_IMGUI_ENABLED <include dirs> file.cpp`.
* Running the editor or game unattended: `HE_HIDDEN_WINDOW=1` (see `docs/headless-runs.md`),
  and never leave a stray `HorizonEditor` running in the background (it eats GPU time
  other runs are measured against).

## 3. Reading budget (this is where the tokens go)

* **Search before you read.** `grep -n` / the Grep tool for the symbol, then read the
  ~100 lines around it. Do not open a file "to get a feel".
* **Never read these whole** (5k-18k lines each): `src/HE_Rendering/src/Backends/*/*Renderer.*`,
  `src/HE_Editor/EditorApplication.cpp`, `EditorHelp.cpp`, `UIEditorPanel.cpp`,
  `ContentBrowserPanel.cpp`, `src/HE_Scene/src/EngineApi.cpp`,
  `src/HE_Core/src/UIWidget/WidgetManager.cpp`, `tests/test_ui_widgets.cpp`,
  `tests/test_material_graph.cpp`. Use offsets/limits.
* **Do not re-derive what a handoff already says.** If a `docs/*` file for your topic exists,
  read its **Handoff** section first (section 5) and start from "Next steps".
* Skip `vendor/` directories entirely (imgui, entt, glm, cgltf...). Their headers are
  only worth a targeted grep for an API signature.
* One subagent per genuinely independent question, with file paths and line ranges in its
  prompt. Do not fan out several agents that will all read the same big file.
* Don't paste whole files or long logs into the conversation; `tail`/`grep` them.

## 4. Conventions that bit us before

* New code in `namespace HE` (see `docs/coding-conventions.md`); exported classes use `HE_API`.
* **Editor UI**: build controls through `EditorWidgets` (buttons, menu items, drop slots).
  Every literal-labelled control needs an entry in `EditorHelp.cpp`, or
  `editor_help_audit.py --check` fails. Row-heavy panels draw only visible rows
  (see `OutlinerPanel.cpp`'s row clipping) — keep a row's height constant.
* **ImGui wrap guard**: `EditorWidgets::WrapText` must be released *before* `ImGui::End()` /
  `EndChild()` (it pushes/pops the wrap position on the current window).
* **UI tests**: `tests/test_*_ui.cpp` drive a real panel headless with `ImGuiSoftwareRaster`;
  set `HE_UI_DUMP_DIR` to get BMP frames and look at them (`scripts/he_uishot.py` converts).
* Scenes and assets are serialized losslessly. A changed chunk layout needs a test that still
  loads the old form; `HAsset.h` (top comment) says when a file-version bump is required and
  when a new tail chunk is enough.
* New sky/cloud shader code: GLSL 4.10 rejects `const` with a non-constant initializer and
  `flat` is a keyword; `kSkyFS` is the single source for GL/Vulkan/D3D, `MetalRenderer.mm` mirrors it.
* Keep comments in the style of the surrounding file; the code base explains the *why*.

## 5. Handoffs: write them, read them

Every topic step ends with a handoff so the next worker does not re-survey the repo.
Template: `docs/handoff-template.md`. Put it as the last section (`## Handoff`) of the step's
doc `docs/<topic>-<what>-<yyyy-mm-dd>.md`, or in the existing plan doc of the topic.

* At **start**: find the topic's latest doc (`ls docs | grep -i <topic>`), read its Handoff
  section, trust it, verify only what you are about to change.
* At **end**: fill the template. File paths with line numbers, what was *verified* (and how)
  versus *not verified*, decisions taken versus only suggested, next steps in order.
* Keep it under ~60 lines. A handoff that is longer than the survey it replaces is no handoff.

## 6. When the Hive is unreachable

Tasks that come from the Hive mention a topic/step/queen and ask you to use the `hive_*` tools
(`hive_join`, `hive_status`, `hive_claim`, `hive_post`, `hive_step_done`, `hive_handoff`, `hive_leave`).

1. Look for the tools **once** (one ToolSearch for `hive`). If they are missing, or the first
   `hive_join` fails (`CONNECT_TIMEOUT`, refused, not found), **stop**. Do not retry in a loop,
   do not "work around it", do not start surveying or editing the repo.
2. Your whole answer is then: `Hive unreachable (<error text>). Nothing claimed, nothing
   changed.` plus the topic/step you were given, so the queen can re-dispatch. No file writes,
   no commits.
3. Exception: the prompt explicitly says to continue without the Hive. Then work normally
   and say in the handoff that nothing was posted to the board.
4. A *later* Hive call failing mid-task (`hive_post`, `hive_step_done`) is not a reason to
   throw work away: finish the step, commit, write the handoff, and report the failed call once.

Tasks that come directly from a person in this conversation are not Hive tasks; this
section does not apply to them.
