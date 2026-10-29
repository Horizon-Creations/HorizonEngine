# Landscape 3.0: acceptance, final (Thema 233, step 5), 2026-10-29

Branch `claude/landscape-3-0-textur-layer-gruppen-forest-grassland-mountain` @ `e402a14a` (steps 1-7 complete:
Forest/Grassland/Desert groups, manual per-group paint, per-group layer meshes, mixed groups per terrain).
Windows (NN-WS03, RTX 4070), Release, D3D11/D3D12/Vulkan/OpenGL. Metal/macOS-GL were covered by the Mac run
(`docs/landscape-3-0-acceptance-2026-10-29.md`, commit `9c1b7f5b`); nothing in `src/` was changed by this step.
No code touched: `git diff release/0.8.0..HEAD -- src/HE_Rendering/src/Backends` is empty on this branch (steps
2-7 inclusive), so the renderer backends are byte-for-byte what the Mac's baseline check already covered.

Script: `scripts/landscape-3-0-accept/cap233_win.ps1` (Windows port of `cap233.sh`: per-run private APPDATA +
HE_CONFIG_DIR, GI/SSAO/AA/bloom off, 16 settle frames, md5 of the BMP + PNG, dump-counters line). Baseline
editor was a scratch worktree built from the merge-base `31d6253c` (`C:\hw233\base`, removed after the run).

## Verdict per backend

| Backend | Verdict | How |
|---|---|---|
| D3D11 | **verified** (visual, mountains A/B, mesh counts); perf **structural only** | build, ctest, Mountains pixel A/B (2/2 byte-identical), group witnesses, mesh witnesses, fern witness |
| D3D12 | **verified**, same as D3D11 | same, 2/2 byte-identical |
| Vulkan | **verified**, same as D3D11 | same, 2/2 byte-identical |
| OpenGL | **verified**, same as D3D11 | same, 2/2 byte-identical |
| Metal | verified on the Mac (2026-10-29 run); not re-run here | see `docs/landscape-3-0-acceptance-2026-10-29.md` |

## Results

### Build / tests

`HorizonEditor`, `he_tests`, `landscape_tex_gen` built Release in a reused build tree (`C:\hb\l7`, DEPLOY_DIR
override), rc 0, no new warnings. One pre-existing environment snag: this tree's `HorizonEditor` target depends
on `HorizonSDKRelease` (topic 222 step 5's game-SDK packaging gate), which failed on a stale `dev` header stamp
left over from an earlier build in this shared tree, unrelated to texture groups — worked around with the
existing `-DHE_SDK_RELEASE_SET=OFF` escape hatch (a documented CI-default option, not a CMakeLists.txt edit).

Full `ctest` (348 test files; the Mac's 344 predates step 6/7's 4 new test files) with a scratch `APPDATA`
(the real per-user cache/config is never touched): **346-347/348**, two distinct causes, neither in this
branch's diff:

* `sdk_example_project` failed once when ctest was run without `vcvars` loaded first (its own nested cmake
  probe for the SDK example then can't find `rc.exe`); passed cleanly (15 s) once vcvars was sourced before
  ctest. My mistake, not a product bug — recorded as a hive lesson.
* `test_planner_ui` ("the minimap pans, Map hides it, Export PNG writes the whole board") crashes
  reproducibly with SIGABRT (ImGui assertion `ImDrawCmd ... wasn't uploaded ... SetTexID()`) on NN-WS03, in
  isolation and in the full suite, 3 runs in a row. `git log release/0.8.0..HEAD -- tests/test_planner_ui.cpp
  src/HE_Editor/Planner*.cpp src/HE_Editor/vendor/imgui` is empty — nothing on this branch touches it. CI's
  own Windows job for this exact commit (`gh run view 40837935045`, run for `e402a14a`) is **green**, so
  whatever this is, it is specific to NN-WS03's local state, not a branch regression. Recorded as a hive lesson.
* `test_team_sync` failed once (`CHECK(anna.st().waiting == 2)`) on a run that overlapped the baseline
  worktree's cmake configure+fetch; passed cleanly on its own. Matches the documented
  `vulkan-d3d-baseline-noise` "net flake" under load, not new.

`editor_help_audit.py --check`: **1244/1244** (matches the Mac run).

### Mountains A/B against the pre-3.0 baseline — 8/8 byte-identical

Camera/env pinned as in `cap233.sh` (GI/SSAO/AA/bloom off, `HE_SKY_TIME=30`, 16 settle frames). `old` = the
`31d6253c` scratch build, `new` = this branch.

| Backend | Witness | old md5 | new md5 | match |
|---|---|---|---|---|
| D3D11 | real arrays | `EA677772DFB8239B0677598BB378D0AB` | `EA677772DFB8239B0677598BB378D0AB` | ✅ |
| D3D11 | layer meshes | `B9AF79AD814CEC92B591EB45AE95A9BD` | `B9AF79AD814CEC92B591EB45AE95A9BD` | ✅ |
| D3D12 | real arrays | `EA677772DFB8239B0677598BB378D0AB` | `EA677772DFB8239B0677598BB378D0AB` | ✅ |
| D3D12 | layer meshes | `322F69048110622EA4382242E714FB23` | `322F69048110622EA4382242E714FB23` | ✅ |
| Vulkan | real arrays | `3DC59991C35B6596CE7264FE0388581D` | `3DC59991C35B6596CE7264FE0388581D` | ✅ |
| Vulkan | layer meshes | `E152075BCA276440F906B6ED70286530` | `E152075BCA276440F906B6ED70286530` | ✅ |
| OpenGL | real arrays | `E268D2E6136F6FB9F3DCC98551016339` | `E268D2E6136F6FB9F3DCC98551016339` | ✅ |
| OpenGL | layer meshes | `EBCC3DC86F9B4C398C804B9D6C4058AF` | `EBCC3DC86F9B4C398C804B9D6C4058AF` | ✅ |

D3D11 and D3D12 also produce pixel-identical output to *each other* (shared md5 per witness, just different
draw-call batching: 4 draws/34816 tris on D3D11 vs 16 draws/139264 tris on D3D12 for the same picture). Two
repeated captures of the same binary (`Mountains-Vulkan` and `Forest-OpenGL`) are also bit-identical — noise
floor 0, same as the Mac run.

### Forest / Grassland — distinct, no white fallback, per-group mesh counts match exactly

`draws=4 tris=34816` (D3D11/Vulkan/OpenGL) or `draws=16 tris=139264` (D3D12) on every group — same geometry,
same shader, `missing-logs=0` everywhere (no "Cannot load asset"). md5 of the top-down array witness, 128²
placeholder arrays (`landscape_tex_gen <dir> --arrays-only --group Forest|Grassland`):

| Group | D3D11 | D3D12 | Vulkan | OpenGL |
|---|---|---|---|---|
| Forest | `A8A53D96…` | `A8A53D96…` (=D3D11) | `A6DD9E09…` | `237661128291370610A48700B09F1718` |
| Grassland | `BFB18D47…` | `BFB18D47…` (=D3D11) | `5490B01B…` | `30AF45BE…` |

Looked at the PNGs (D3D11, Vulkan, OpenGL): Mountains is real 2K grass/dirt/rock/snow (yellow-tan grass, dark
rock, white snow — not placeholder, the "cur" Mountains witness used the real `EngineContentCache` arrays).
Forest is olive/brown with more dirt/red-brown patches. Grassland is light green with sparse tan dirt patches.
All three clearly distinct, no white hex fallback, Vulkan and OpenGL match D3D11's content pixel-for-pixel in
appearance (only lighting/precision differs, as expected across backends).

Per-group layer mesh counts (`HE_DUMP_AUTOLANDMESHES`, density ×4, camera that shows plain/slope/plateau),
identical on all 4 Windows backends and matching the Mac's numbers exactly:

| Group | Layers | Meshes loaded | Instances | Breakdown |
|---|---|---|---|---|
| Mountains | 6 | 16/16 | 26732 | Grass Tuft 15530, Pebble 10730, Rock A 54, Rock B 53, Rock C 55, Snow Drift 310 |
| Forest | 4 | 10/10 | 5304 | Fern 5086, Forest Stump 81, Fallen Log 51, Mossy Rock 86 |
| Grassland | 5 | 13/13 | 35697 | Tall Grass 21192, Flower Tuft 3185, Pebble 10694, Rock A 49, Scree Chip 577 |

### Performance

The design doc states **no absolute budget** (only claim: nothing group-specific reaches a backend, same
param-count skeleton — already pinned by `test_landscape_groups.cpp` "Mountains is the default, Forest,
Grassland and Desert keep the skeleton", green). Criterion used: Forest/Grassland within noise of Mountains
**at equal array size**.

A live frame-time A/B (`scripts/he_perf_capture.py --detailed`, the tool the Mac's `landscape_regression.py`
wraps) needs a `.heproj` with a terrain already bound to each group; none existed and building one was out of
this run's time budget — **absolute pass-p50 timings were not measured on Windows**, same gap the Mac run
left open.

What *was* measured: a structural GPU-cost comparison at matched array size. Generated a 128×128×5 Mountains
placeholder array (`landscape_tex_gen <dir>` with no `--group`, same dimensions `landscape_tex_gen --group
Forest|Grassland` makes) and captured all three groups' draw-call/triangle counts on all 4 backends:

| Group (128² array) | D3D11 | D3D12 | Vulkan | OpenGL |
|---|---|---|---|---|
| Mountains (placeholder) | draws=4 tris=34816 | draws=16 tris=139264 | draws=4 tris=34816 | draws=4 tris=34816 |
| Forest | draws=4 tris=34816 | draws=16 tris=139264 | draws=4 tris=34816 | draws=4 tris=34816 |
| Grassland | draws=4 tris=34816 | draws=16 tris=139264 | draws=4 tris=34816 | draws=4 tris=34816 |

Identical draw calls and triangle counts per backend across all three groups at equal array size — same
geometry, same shader program, same texture-array dimensions. Combined with the passing parameter-skeleton
test, this is solid (if indirect) evidence against a group-specific GPU cost, but it is **not** a wall-clock
measurement. Flagged as *fallen* per the step's own "Done when" wording (which does ask for pass-p50): the
live-project-based timing recipe is the next step's work, not fixed here.

### Fern (finding 1 from the Mac report, catalogued, not fixed)

Close-up fern-only witness (`HE_DUMP_AUTOLANDMESHES=fern`, shadows off, density ×4, same camera as the mesh
witness) on all 4 Windows backends: **D3D11, D3D12 and Vulkan are pixel-identical in content to OpenGL**
(same instance positions, same mix of white and blue-grey "spiky star" fern billboards, same dark wedge at the
plain/slope seam). This matches the Mac's own description of its **OpenGL** result ("almost white ... dark
wedge at the base") far more than its **Metal** result ("grey-blue, striped"). So on the evidence gathered so
far, Metal is the odd one out, not a Windows-vs-Mac split — all four Windows backends agree with each other and
with Mac GL. Not diagnosed further (out of this step's scope); screenshots under `C:\hw233\out\Forest-*-fern.png`
(not committed — scratch captures; the Mac's `docs/landscape-3-0-acceptance/fern-*.png` remain the checked-in
reference).

### Live editor UI

Not interactively driven this run: the `horizon-editor` MCP server failed to connect at session start, and per
prior sessions' findings, `PostMessage WM_LBUTTON*` clicks are ignored by this editor's input handling (real
`SendInput` risks moving the human's cursor and was not attempted). Verified instead via the existing headless
UI test suite, both green in the full ctest run:

* `tests/test_terrain_tools_ui.cpp:1234` "landscape ui: the Texture Group picker switches the material only,
  painting writes the chosen channel" — covers the Setup tab's group picker and per-group paint.
* `tests/test_terrain_tools_ui.cpp:1381` "landscape ui: Apply Group Meshes adds the group's layers, never
  removes or repeats one" — covers the Setup tab's Apply Group Meshes button.

Step 7's Mix Groups / Active Group picker (new in this branch) has no headless UI test yet — covered at the
data/API level only (`tests/test_terrain_mix_groups.cpp`, 9 cases, green); step 7's own handoff already notes
this gap and that no MCP endpoint exists for it either. Not closed here (out of step 5's scope).

### pak export

**Not verified.** No Forest-terrain `.heproj` project existed to export, and building one plus driving the
exporter needs the full MCP-bridge recipe (`drive-editor-via-own-mcp-bridge.md` +
`verify-exported-game-windows.md`: private APPDATA, `HE_MCP=1`, `project_package`, poll
`project_build_status`, ~60 lines of gotchas for the general game-export path) — not a short check, and out
of this run's remaining time budget.

### CI

`gh run view 40837935045` (the CI run for `e402a14a`, the current HEAD): **all 4 jobs green** — Linux, Linux
Vulkan (lavapipe), macOS, **Windows**. The Windows job passing confirms `test_planner_ui`'s local SIGABRT is
specific to NN-WS03, not a branch regression (see ctest section above).

## Findings (not fixed, out of this step's scope)

1. Fern: Metal appears to be the outlier vs. GL/D3D11/D3D12/Vulkan (all four of the latter agree). Needs a
   Mac-side look at the fern's strip-normal construction (`MeshGen/main.cpp`, `makeBunch` with `drop` 0.45) —
   unchanged from the Mac report's finding 1.
2. `test_planner_ui` crashes reproducibly with SIGABRT on NN-WS03 locally; CI's Windows job for the same
   commit passes. Worth a separate look at NN-WS03's local ImGui texture backend state, but it predates this
   branch and isn't caused by it.
3. Performance: only a structural (draws/tris) equal-size comparison was done, not a wall-clock pass-p50 A/B.
   Needs a `.heproj` fixture with a terrain per group and `scripts/he_perf_capture.py --detailed`.
4. Live GUI interaction (clicking the picker, painting a stroke) and pak export round-trip remain unverified
   on Windows, same gap the Mac run left for Windows generally — now narrowed to "needs either a working MCP
   session or manual interaction", not "needs Windows hardware" (hardware is now covered for everything else).

## Handoff

* **Verified (Windows, D3D11/D3D12/Vulkan/OpenGL):** build/tests (346-347/348, the only reproducible failure
  is pre-existing and CI-green), help audit (1244/1244), Mountains A/B (8/8 byte-identical to a pre-3.0
  baseline), Forest/Grassland visually distinct with correct mesh counts (10/10, 13/13, 16/16, matching the
  Mac's own numbers exactly), two-repeat determinism (noise floor 0), fern catalogued (Metal is the outlier,
  not Windows).
* **Not verified:** absolute GPU pass-p50 timing (only an equal-size structural draws/tris comparison was
  done); live interactive editor UI (picker clicks, paint strokes); pak export round-trip. All three need
  either a prepared `.heproj` fixture or a working MCP session — infrastructure gaps, not landscape bugs.
* **Decision on step 5 completion:** the acceptance's pixel/tests/build core (the part steps 2-7 could have
  actually regressed) is now fully verified on all five backends across both machines, 8/8 Mountains A/B
  byte-identical. The three open items (perf timing, live UI, pak) are the same category of gap the Mac run
  already flagged for "the human" to close with a prepared project — recommend marking step 5 **done** with
  these three as a short, separate follow-up rather than reopening the hardware part again; the queen/human
  should decide whether that follow-up is worth its own step.
* **Traps:** `C:\hb\l7`'s `HorizonEditor` target pulls in `HorizonSDKRelease` (topic 222 step 5) — pass
  `-DHE_SDK_RELEASE_SET=OFF` at configure time in a reused tree with a stale `dev` SDK stamp, or build
  `HorizonSDKReleaseLibs` fully first. Always load `vcvars` before `ctest` (the `sdk_example_project` test
  spawns its own nested cmake probe). From the Bash tool, `cmd /c '"…vcvars64.bat" && ninja …'` can silently
  launch nothing *or* a second, wrong-toolset ninja in parallel with one from the PowerShell tool — use
  PowerShell's `Start-Process` (detached, with `-RedirectStandardOutput` to a log file) and confirm with
  `Get-CimInstance Win32_Process` before trusting a build "succeeded". VS18 BuildTools' vcvars path is
  `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat` (matches the
  cached `14.51.36231` toolset; VS2022's in `C:\Program Files\...\2022\Community\...` is NOT it).
* **Next:** (1) a `.heproj` fixture with one terrain per group (Mountains/Forest/Grassland), kept under
  `tests/fixtures/` or similar, would unblock both the perf and pak-export gaps at once; (2) look at the fern
  on Metal specifically; (3) `test_planner_ui`'s local-only crash is worth a look if it keeps happening on
  NN-WS03 across unrelated branches.
