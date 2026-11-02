# Auto Landscape Material 2.0: verification (Thema 180, step 11)

2026-10-21 · macOS (Apple Silicon, Metal), Release tree `cmake-build-release`, branch
`claude/auto-landscape-material-2-0-alle-backends-bcn-instancing-audit` @ `ab87f6cb` (code identical to `fbc999fd`, the tip CI judged: `ab87f6cb` only edits one audit-doc line).
No feature or shader code was changed in this step.

## Build (foreground)

| Command | Exit |
|---|---|
| `cmake --build cmake-build-release --target HorizonEditor HorizonGame asset_compiler he_tests -j 8` (the Bash call hit its 600 s cap and carried on; `BUILD_RC=0` was read from the finished command, last line `[370/371] Linking CXX executable tests/he_tests`, 0 `error:` lines in the log) | 0 |
| same command again (nothing to do; the SDK stamp check printed `dev ok` / `release ok`) | 0 |

Products exist: `src/HE_Editor/HorizonEditor`, `src/HE_Game/HorizonGame`, `src/HE_Tools/asset_compiler`, `tests/he_tests`.

## Tests

`cd cmake-build-release && ./tests/he_tests` (whole binary): exit 1, `test cases: 5674 | 5671 passed | 3 failed | 16 skipped`, `assertions: 1692926 | 1692923 passed | 3 failed`.
The three failures are the known pre-existing ones, nothing else:

- `tests/test_hpak.cpp:1188` `scanContentDirectory() == 2` gets 6 (also before this topic, see the step 10 doc).
- `tests/test_profiler.cpp:107` and `:215` (load-sensitive); `--source-file=*test_profiler.cpp` alone: 18/18, 200374 assertions, exit 0.

Topic files, each run alone with `--source-file=*<file>.cpp`, all exit 0:

| File | Cases | Assertions |
|---|---|---|
| `test_layer_mesh_instancing.cpp` | 6/6 | 229 |
| `test_texture_bcn.cpp` | 12/12 | 343 |
| `test_auto_landscape_variants.cpp` | 8/8 | 8354 |
| `test_foliage_rules.cpp` | 11/11 | 9196 |

`python3 scripts/editor_help_audit.py --check`: exit 0, 1212/1212 controls, 1978 help entries.

`tests/CMakeLists.txt` lists `test_auto_landscape_variants.cpp` (:11), `test_layer_mesh_instancing.cpp` (:205), `test_texture_bcn.cpp` (:314). The BC7 cook tests only run with `HE_ENABLE_BC7` (:1103).

## D3D11 / D3D12 / Vulkan

Not buildable on this Mac. Sources changed against `origin/release/0.7.0` (`git diff --stat origin/release/0.7.0...HEAD -- src/HE_Rendering`): `D3D11Renderer.cpp`, `D3D12Renderer.cpp`, `VulkanRenderer.cpp` (plus Metal, OpenGL). CI run 39548430947 on `fbc999fd` is `success` for Windows, macOS, Linux and Linux · Vulkan (lavapipe), so those files **compile** on the Windows and Linux toolchains, and the lavapipe jobs `Vulkan image probe (Xvfb)` and `Vulkan image tests (offscreen)` passed. The Windows step `Layer-mesh image test (D3D11, D3D12)` shows `success`, but it is `continue-on-error`, so this does not prove that it rendered; the artifact `layer-mesh-shots-windows` was not opened here. No D3D/Vulkan hardware result is claimed.

## Docs brought up to date

- `README.md` texture-compression row kept, with two corrections: CPU decoders exist for BC1/BC3/BC4/BC5 only (not BC7), and the CI sentence now names what CI actually did (compile Windows/Linux/lavapipe). Claims checked: `test_texture_bcn` exists and passes; the OpenGL probe is in the step 10 doc (`scripts/bcn-gl-probe/probe.cpp`); export still picks one format per pack.
- `docs/auto-landscape-material-2-plan.md` §10 has a status column per step, §11 replaces the stale "CI is red" line.
- Step 10 and step 4 docs: the stale "not compiled / never drawn" items carry an update note.
- `mem2x-V3-bc3-r1.conditions.txt` (flagged "changed"): tracked since `fbc999fd` (audit raw data), nothing to do.
- `build_errors.txt` in the repo root is tracked and was not touched.

## Handoff

- **Topic / step:** 180, step 11 of 11
- **Branch / commit:** `claude/auto-landscape-material-2-0-alle-backends-bcn-instancing-audit` @ see `git log` (pushed: see the result post)
- **Done:** full foreground build of the four targets exit 0; `he_tests` full run with only the three known failures; topic tests green alone; help audit green; docs and README reconciled.
- **Files touched:** `README.md:60`, `docs/auto-landscape-material-2-plan.md` §10/§11, this file, two Not-verified notes in the step 4 and step 10 docs.
- **Verified:** commands and exit codes above.
- **Not verified:** D3D11/D3D12/Vulkan rendering (CI compile only), BCn uploads on any GPU, the Windows layer-mesh image step actually rendering, hardware smoke test on NN-WS03 (**open**), OpenGL live perf capture, D3D/Vulkan timings, a paint-stroke re-scatter cost.
- **Decisions taken:** kept the README edit (corrected); no code change, since nothing regressed.
- **Decisions suggested, not taken:** the three pre-existing reds (`test_hpak` scan count, `test_profiler`) should get their own topic; open the `layer-mesh-shots-windows` artifact before trusting the Windows step.
- **Pending (not this step):** surface material for the layer meshes (plan §8.5), BC5 normal Z reconstruction (§7.3) before any BC5 normal array ships, per-map BCn choice and route B (§7.5), real V = 3 photo arrays on the EngineContent server (not git, 100 MiB limit), stale pin tables in `MaterialGraph.h` and `auto-landscape-material-textures.md` §8.
- **Traps:** `cmake --build` over the 600 s cap keeps running in the background and the notification's exit code belongs to the wrapping shell, so read `BUILD_RC` from the output; run `he_tests` from the build directory.
- **Next steps:** 1. queen merges into `release/0.7.0`; 2. Windows smoke test on NN-WS03 once online; 3. the pending list above.
