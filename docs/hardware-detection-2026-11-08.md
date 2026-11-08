# Hardware detection: the engine knows the machine, scripts and the editor can show it

Hive topic 337 (builds on 336: the settings rows, the Quality page, the quality presets). Done 2026-11-08 on NN-WS03
(Windows 11, Ryzen 9 9950X, RTX 4070), branch `claude/hardware-erkennung-api` (cut from
`claude/quality-windows-backends`, so it carries topics 324/336 and the Windows backend work). No pull request.

## 1. What exists now

| Piece | Where |
|---|---|
| `HE::HardwareInfo` (CPU, RAM, OS, battery; GPU and display handed over later), `hardwareInfo()`, `freeMemoryMB()`, `onBattery()`, `hardwareSummary()`, the `HardwareProvider` test seam, `HardwareProperty` | `HE_Core/include/Diagnostics/HardwareInfo.h`, `src/Diagnostics/HardwareInfo.cpp` |
| The CPU / RAM / OS probes (moved out of `Log.cpp`; the startup banner and `Log::systemInfoBlock` read the same struct) | same |
| `IRenderer::GetAdapterInfo()`, filled by every backend's `Initialize` | `IRenderer.h`; D3D11/D3D12 through `D3D_Shared/DxgiAdapterLog.h`, Vulkan `pickPhysicalDevice`, OpenGL's GPU block, Metal, Software |
| Hand-over of GPU and display, five `Hardware` lines in the log | `Application.cpp`, after the renderer is up |
| 26 `hardware.*` registry rows (HorizonCode, Lua, Python, C++) and the built-in enum `engine:HardwareProperty` | `HE_Scene/src/EngineApi.cpp`, `TypeRegistry.cpp`, node docs in `HcNodeDocs.cpp` |
| The editor page Preferences » Preview » Hardware | `HE_Editor/HardwarePage.{h,cpp}`, `EditorSettingsPanel.cpp` |
| Suggested preset: `recommendQualityPreset` + reason, rows `hardware.recommendedPreset` / `...Reason`, a hint on the Quality page | `QualitySettings.h`, `ProjectSettings.cpp` |
| Manual section "Hardware Info" (rendering page, after Quality Settings) | `EditorDeps/Docs/he-docs.json` |

Only type and model names and sizes: no serials, device ids, MAC addresses or user names, and nothing is sent anywhere.

## 2. The values

| Group | Fields | Where they come from |
|---|---|---|
| Processor | name, physical cores, logical threads, architecture | Windows: `cpuid` brand string, `GetLogicalProcessorInformationEx`, `hardware_concurrency`. macOS: `sysctl machdep.cpu.brand_string`, `hw.physicalcpu`. Linux: `/proc/cpuinfo` (cores = distinct physical id / core id pairs, 0 when the board lists none) |
| Memory | installed MB, free MB (fresh on every read) | `GlobalMemoryStatusEx` / `sysctl hw.memsize` + `host_statistics64` / `sysconf` + `/proc/meminfo` |
| Graphics card | name, vendor, driver, video memory, API | the running renderer (below) |
| Display | width, height, refresh, scale, name | SDL: the display the window is on, read right after the renderer is up |
| System | OS name, OS version line, battery present, on battery | Windows `RtlGetVersion`, `GetSystemPowerStatus`; macOS `uname` + `kern.osproductversion`, `hw.model` containing "Book"; Linux `/etc/os-release`, `/sys/class/power_supply` |

Graphics card per backend: D3D11 / D3D12 ask DXGI (name, vendor id, dedicated memory, driver; NVIDIA's own number is built
from the Windows driver version: `32.0.16.2101` becomes `621.01`); Vulkan reads `VkPhysicalDeviceProperties` and the
device-local heap (NVIDIA's packed driver version is decoded, Intel's on Windows too); OpenGL reads `GL_RENDERER` (cut before the
first slash) and `GL_VERSION` (the part after the vendor), video memory only through `GL_NVX_gpu_memory_info`, otherwise 0; Metal takes the
device name and its recommended working set (shared memory); the software renderer says "CPU rasterizer". A field a platform cannot
answer stays empty / 0; the editor page shows a dash and the rows return `""` / `0`, never a guess.

## 3. Checked on this machine

The five log lines of each backend against Windows' own tools (`Get-CimInstance`):

| | engine | Windows |
|---|---|---|
| CPU | AMD Ryzen 9 9950X 16-Core Processor, 16 cores, 32 threads | same (Win32_Processor) |
| RAM | 31859 MB | 31860 MB (rounding) |
| GPU / driver | NVIDIA GeForce RTX 4070, 621.01 (`32.0.16.2101`) | same name; DriverVersion 32.0.16.2101 |
| Display | 2560x1440 @ 60 Hz, scale 1.25 | 2560x1440, 59 Hz (59.94 shown as 59), scale 125 % |
| OS | Windows 11 (10.0 build 26300) | Windows 11 Pro, build 26300 |

The GPU line on the four backends: `NVIDIA GeForce RTX 4070 (12010 MB, driver 621.01, D3D11 / D3D12 / Vulkan)` and
`(12282 MB, driver 621.01, OpenGL)`. The video memory differs by source: DXGI and Vulkan report 12010 MB, the NVIDIA GL extension
12282 MB; neither is "wrong", they count differently.

Tests (`he_tests`, this tree): `test_hardware_info` 13 cases (module, provider seam, merge, summary, vendor names, the 26 rows
over a fake machine and over the real one, by name / by enum, the built-in enum, the preset thresholds, reason line),
`test_horizoncode_codegen` (new parity fixture `hardware_rows`: the interpreted and the compiled half read the same machine),
`test_scripting_binding` (Lua about box), `test_python_scripting` (Python about box), `test_ui_shot` (the Hardware page, picture below),
`test_docs_library` (the manual section), `test_engine_api*`, `test_hc_node_docs`, `test_logging` (the startup block unchanged).
`editor_help_audit.py --check`: 1277 / 1277. The whole `he_tests` suite ran green at the step-5 state (6220 / 6220 cases, 2 155 532
assertions, 20 skipped); afterwards only the manual section, its test and the real-machine row test were added, and those files re-ran
green (`test_docs_library` 17 / 17, `test_hardware_info` 13 / 13, `test_ui_shot` 51 / 51, `test_editor_help` 19 / 19). `HorizonEditor` builds.

![The Hardware page](hardware-detection-page-2026-11-08.png)

(A made-up machine through the provider seam, so the picture does not depend on the PC it was made on.)

## 4. The suggested preset

`recommendQualityPreset` is a coarse guess, shown as a suggestion and never applied: video memory sets the ceiling (10000 MB and
up Ultra, 6000 High, 3000 Medium, below Low; unknown memory at most Medium; Apple GPUs by RAM: 32 GB Ultra, 16 GB High), RAM and
threads cap it (Ultra 16 GB + 8 threads, High 12 GB + 6, Medium 8 GB + 4), no usable GPU or the software rasterizer reads Low. On this
PC (12010 MB, 31859 MB, 32 threads): Ultra. It is the lower of the two answers. A measured recommendation (timing the presets) is
the follow-up topic and would replace it; the timings in `docs/quality-windows-backends-2026-11-08.md` show what such a benchmark
would see (for example D3D11 High is dominated by software GI).

## 5. Not done / not verified

* macOS, Linux and the Metal adapter code are written but were not built or run (this is a Windows machine): the `sysctl` /
  `/proc` / `MTLDevice` paths, macOS battery (`onBattery` is always false there: no IOKit link), `host_statistics64` free memory.
  The Mac in the Hive can verify them; the unit tests there will run the same fake-provider cases.
* Display changes after start (window moved to another monitor, resolution change) are not tracked: the display is read once, when the
  renderer is up. Not asked for here; `setDisplayInfo` is the hook if a later topic wants it.
* The Website manual (`rendering.html`) and the generated scripting-reference page were not touched (separate repo / generated); the
  in-engine manual has the section.
* A "game settings menu" example as a ready-made graph: the Lua and Python samples in the manual and their tests are the working
  examples.
* Multi-GPU machines: the card is the one the renderer runs on, no list of all adapters.
* A visible-window run of the editor with the new page open was not done; the page is checked headless (UI test, picture) and its
  data on the real machine through the log lines of all four backends.

## 6. Files

`Diagnostics/HardwareInfo.{h,cpp}`, `Log.cpp`, `IRenderer.h`, `DxgiAdapterLog.h`, the five renderers' `Initialize`, `Application.cpp`,
`TypeRegistry.{h,cpp}`, `EngineApi.{h,cpp}`, `HcNodeDocs.cpp`, `QualitySettings.h`, `ProjectSettings.cpp`, `HardwarePage.{h,cpp}`,
`EditorSettingsPanel.{h,cpp}`, `ProjectSettingsPanel.cpp`, `EditorHelp.cpp`, `EditorDeps/Docs/he-docs.json`, tests and the
codegen fixture (`tests/CMakeLists.txt` lists the generated class `HardwareRows`).
