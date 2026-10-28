# Audit-Optimierung: Vulkan-Backend (Stufe 3)

Thema 215, from `docs/audit/AUDIT.md` Stufe 3 and `docs/audit/modul_rendering.md`
(REN-802, REN-803, REN-808).

## Step 1: REN-802/803 (upload path)

**Before.** Every mesh, skeletal mesh and texture the draw loop saw for the first
time made its own staging buffer and its own one-shot command buffer, submitted it
and waited in `vkQueueWaitIdle`. A scene of N assets drained the GPU N times in the
frame that first drew it. Editor icons (`CreateImGuiTexture`) also did a
`vkDeviceWaitIdle` each. Static vertex and index buffers were host-visible, each
with its own `vkAllocateMemory` (the driver limit is often 4096 allocations).

**Now** (`VulkanRenderer.h`, block "Asset upload path"; `VulkanRenderer.cpp`
7284-7650):

* **Upload batch.** `uploadRGBA8Image`, `uploadTextureImage`, `createMeshBuffers`
  and `resolveSkeletalMesh` record their copies into ONE open upload command buffer
  (`uploadCmd`) and return right away. The bytes come from a persistent, mapped
  32 MB staging ring (`HE::vkup::StagingRing`, `VulkanUploadAlloc.h`). An upload
  bigger than the ring, or one that finds the ring full of copies the GPU has not
  finished, gets a staging buffer of its own, freed with its batch. It never waits.
* **Ordering.** Every `vkQueueSubmit` in the file goes through `submitGraphics`, and
  every `vkDeviceWaitIdle` through `deviceWaitIdle()`. Both submit the open batch
  first, with a fence of its own. It is the same queue, so submission order and the
  batch's closing barrier (TRANSFER_WRITE → ALL_COMMANDS) make the copies visible to
  whatever is drawn next. A batch's ring bytes and staging buffers are released once
  its fence has signalled (`retireUploads`). The image read barriers moved from
  FRAGMENT_SHADER to ALL_COMMANDS, because the old wait used to hide vertex and
  compute reads.
* **Device-local meshes.** Static and skeletal mesh buffers are DEVICE_LOCAL, bound
  at an offset into 64 MB blocks with a first-fit free list (`HE::vkup::RangeAllocator`).
  Each mesh keeps its own `VkBuffer`, so the ~15 bind/draw sites are unchanged.
  Buffers over 16 MB get a dedicated allocation. An empty block goes back to the
  driver. Upload always goes through staging, also on UMA/lavapipe, so the path CI
  runs is the one a dGPU runs.
* **Destroy-before-submit.** The new path has one rule: nothing may be destroyed
  while a copy into it is still unsubmitted. `resolveAndUploadAlbedo` now allocates
  its descriptor set before the upload, where it used to destroy the image when the
  pool was full. The upload functions create their view and stage their data before
  they record anything. The two allocation-failure exits wait first. Every other
  destroy (Shutdown, `processPendingInvalidations`) sits behind `deviceWaitIdle()`.
* **ImGui icons** use `uploadRGBA8Image` with no device idle. They flush (submit,
  no wait) right away: with multi-viewport on, `imgui_impl_vulkan` submits the
  platform windows itself, past `submitGraphics`.
* **Switches and numbers.** `HE_VK_UPLOAD_SYNC=1` brings back the wait after every
  upload, in the same binary (for A/B, or as a way out). `HE_VK_UPLOAD_STATS=1` logs
  every batch. At shutdown, one line `VulkanRenderer: uploads — N meshes, M textures,
  X MB in B batches (…); … ms CPU in upload calls, W waits / … ms waiting` is always
  logged, and `he_vk_imagetests.py` prints it for every shot.

**Unchanged on purpose:** the other one-shot submits with their wait (moon,
UI font atlas, SSAO noise, GI prime/BLAS, sky cube, world preview). They run at
startup or on rare events and are not part of the asset load path. Images keep one
dedicated allocation each, because six destroy paths free image memory directly.

### Measurement

There is no Vulkan device on the Mac this step ran on, so this step has **no
load-time number** yet. Two kinds of witness are in place:

* lavapipe (CI job `vulkan-lavapipe`): the cases `clustered` and `ui_image` have a
  `sync` variant (`HE_VK_UPLOAD_SYNC=1`), which must match the batch path within
  mean |Δ| 0.05. Meshes come from the clustered scene and a texture from the UI
  image. Each shot also logs its upload line, so the run shows waits and CPU ms for
  both variants side by side. That is software rendering, not a load-time figure.
* dGPU (NN-WS03): measured in step 2, see "Load time on NN-WS03" below.

## Step 2: REN-808 (`VkPipelineCache`)

**Before.** All 34 `vkCreateGraphicsPipelines`/`vkCreateComputePipelines` call sites (32 + 2)
passed `VK_NULL_HANDLE` as the cache, and ImGui made its own pipelines with none.
Whether a start was fast depended only on the driver's private disk cache.

**Now** (`VulkanRenderer.h:741`, `VulkanRenderer.cpp:7643-7800`):

* `createPipelineCache()` runs right after `createDevice()` (`:191`). It reads
  `<pref path>/vkpipelinecache/<vendor>_<device>.bin`, the folder next to
  `glprogcache`/`d3dshadercache`, with one file per GPU. `savePipelineCache()`
  (`:405`, before `vkDestroyDevice`) writes it back when the blob changed. It
  writes a temp file and renames it over the old one.
* The file format is in `VulkanPipelineCacheFile.h` and has no Vulkan dependency:
  our header (magic, vendor, device, driverVersion, pipelineCacheUUID, size,
  hash64), then the driver blob unchanged. A blob is handed to the driver only if
  our header repeats the device, the size and hash fit, and the driver's own
  `VkPipelineCacheHeaderVersionOne` names the same device. Anything else is a miss
  (start empty, rewrite at shutdown). If the driver refuses data that passed these
  checks, the renderer retries with an empty cache. The rules are pinned in
  `tests/test_vulkan_pipeline_cache.cpp`.
* All 34 call sites go through `createGraphicsPipelines`/`createComputePipelines`,
  which pass `m_pipelineCache` and time the call. ImGui gets the same cache
  (`EditorApplication.cpp:1363`, `GetPipelineCache()`).
* Switches: `HE_VK_PIPELINE_CACHE=0` turns the cache off (the old path, same
  binary). `HE_VK_PIPELINE_CACHE_DIR=<dir>` moves the file. `SDL_GetPrefPath`
  ignores `%APPDATA%`, so a scratch APPDATA does not isolate it.
* Log lines: `[startup] Vulkan pipeline cache: <loaded|empty|hash mismatch|…>, N KB from <path>`,
  `[startup] Vulkan Initialize: ms= pipelines= pipelineMs= cache=`, and at shutdown
  `VulkanRenderer: pipelines — N created, X ms in vkCreate*Pipelines; cache … (in, out, written|unchanged)`.

**Not done (REN-808's other half):** the sky fragment shader is still compiled
GLSL→SPIR-V by shaderc on every start, and `BuildSkyNoise3D(256)` still bakes on
every start. Neither goes through a pipeline cache. Pipelines are still built on
the render thread, not in a job.

### Load time on NN-WS03 (RTX 4070, driver 610.88, Release)

Harness: `scripts/vulkan-startup/series.ps1` (calls `measure.ps1`, a sibling of the D3D one). The
editor runs hidden, quits after 300 frames, and uses a private `-CacheDir`. The
project is a copy of HEProject: 257 meshes, 27 textures, 61 MB, 38 pipelines.
There are 3 runs per condition, interleaved, and the tables give the median.
`-NoValidation` (`VK_LOADER_LAYERS_DISABLE=*validation*`) was set, because on this
branch the validation layer is still forced on (`createInstance`,
`gpuDebug = true`). REN-804 from Thema 212 is not on `release/0.7.0`/`main` yet.
A second series with validation on gave the same picture: driver-cold
pipelines 1.63-1.95 s.

**Caveat:** a game ran on the same GPU the whole time (99 % load). Absolute ms
are noisy, but the ratios held in every run.

| Condition (pipeline cache / NVIDIA driver cache) | `vkCreate*Pipelines` | Renderer `Initialize` | First frame since `Run` |
|---|---|---|---|
| off / cold | 1730 ms (1646-2013) | 2556 ms | 3050 ms |
| off / warm | 6.9 ms | 810 ms | 1242 ms |
| empty / cold (very first start) | 1644 ms | 2274 ms | 2664 ms |
| **loaded / cold** | **43 ms (39-47)** | **985 ms** | **1422 ms** |
| loaded / warm | 3.9 ms | 823 ms | 1264 ms |

"Driver cold" means a fresh copy of the exe under a new name: NVIDIA keys its
Vulkan cache (`%LOCALAPPDATA%\NVIDIA\DXCache\*.nvph`) by exe **name**. The
series deletes the copies and every DXCache file that appeared during it. On 2026-10-18 that
was 18 files per series; new files of another process from those minutes (a D3D bee, the game) may have gone
too. `__GL_SHADER_DISK_CACHE=0` does
**not** switch that cache off on Windows: a run with it set was just as fast
and still updated the `.nvph`.

**Reading.** On NVIDIA the driver's own cache already makes a repeated start
fast, so `VkPipelineCache` saves ~1.6-1.7 s only where that cache is cold for
this exe, while ours is warm. That happens with a renamed or moved exe (every
exported game gets its own name), an evicted or deleted driver cache, and
drivers with no implicit cache. After a driver update both caches are invalid
(our header holds driverVersion and UUID). One cost: an *empty* cache next to a
*warm* driver cache took ~50 ms instead of ~6-18 ms (bare editor, two runs).
That happens once, on the first start after our file is lost.

**Upload path (step 1, REN-802/803), same series, cache warm:**

| | batches | waits / waiting | CPU in upload calls | First frame |
|---|---|---|---|---|
| `HE_VK_UPLOAD_SYNC=1` (old) | 284 | 284 / 212 ms | 228 ms | 1314 ms |
| batch (new) | 28 | 0 / 0 ms | 16 ms | 1170 ms |

The batch path logged **104 oversize staging buffers**: 61 MB in the first frame
do not fit the 32 MB ring while its copies are unsubmitted. It never waits, but
it allocates 104 buffers. See Next steps.

**Persistence witness (bare editor):** run 1 logs `empty … written` (873 KB), run 2
`loaded` (51.0 → 5.5 ms). After one payload byte was flipped, run 3 logged `hash mismatch`,
exited 0 and rewrote the file. The file name `10de_2786.bin` shows the RTX ran, not
the AMD iGPU.

## Handoff

- **Topic / step:** 215, step 2 of 2 (topic complete from my side)
- **Branch / commit:** `claude/audit-optimierung-vulkan-backend-stufe-3`, see the hive post for the final commit (pushed: yes)
- **Done:** REN-808 pipeline half: persisted `VkPipelineCache` for all 34 create sites + ImGui. dGPU numbers for steps 1 and 2 are above.
- **Files touched:** `src/HE_Rendering/include/Backends/Vulkan/VulkanPipelineCacheFile.h` (new); `VulkanRenderer.h:51,741`;
  `VulkanRenderer.cpp:191,225,405,7643-7800`, 34 call sites (sed: `vkCreate*Pipelines(m_device, VK_NULL_HANDLE,` → wrapper);
  `src/HE_Editor/EditorApplication.cpp:1363`; `tests/test_vulkan_pipeline_cache.cpp` + `tests/CMakeLists.txt`; `scripts/vulkan-startup/measure.ps1` + `series.ps1`.
- **Verified:** Windows Release build (MSVC, `F:/hw215`), no new warnings. `he_tests --source-file='*test_vulkan_pipeline_cache.cpp'` passes 4/4 (21 assertions).
  Negative control (standalone `cl` build against a header without the driverVersion and hash checks): 2/4 fail, as expected.
  `test_vulkan_upload_alloc` passes 6/6. Full `he_tests` (scratch APPDATA): 5532/5537. Red are the 3 `test_input_gamepad` cases (known on NN-WS03, a real pad is attached) and 2 `test_profiler` cases, which pass on their own (18/18), so they are flaky under load. Runtime on the RTX 4070 as above, every run exit 0.
  Validation messages (layer on, 7-8 per run, all at shutdown after "shutting down ImGui": vkDestroyBuffer/Pipeline/Sampler in use)
  are the same set with cache off and on, so they do not come from step 2. They are ImGui's own objects:
  `OnShutdown` calls `ImGui_ImplVulkan_Shutdown()` with no device wait before it. `HE_VK_UPLOAD_SYNC=1` still runs the
  step-1 code, so the series says nothing about step 1. Not checked against the pre-step-1 base; step 1's lavapipe CI job was green.
- **Not verified:** Linux/macOS/lavapipe runtime of step 2 (CI). AMD/Intel drivers. The exported game (HorizonGame has no ImGui; the renderer path is the same).
- **Decisions taken:** one file per GPU (vendor_device), last writer wins when two editors run (costs warm-up, never correctness);
  the cache is saved only at shutdown (a crash loses that session's new pipelines); ImGui shares the cache.
- **Decisions suggested, not taken:** ship a pre-warmed cache with an exported game; cache the sky SPIR-V and the noise volume by input hash; build pipelines in a job.
- **Traps:** a new `vkCreate*Pipelines` must use the wrappers, otherwise it bypasses the cache and the timing. `__GL_SHADER_DISK_CACHE` is ignored on Windows.
  The validation layer is forced on until Thema 212 (REN-804) is merged, so timings without `-NoValidation` include it. Drive C: on NN-WS03 was full; build on F:.
- **Next steps:** 1. CI on this branch (lavapipe exercises the cache with Mesa). 2. Staging ring: flush the batch when the ring is full, or size it to the frame's uploads
  (104 oversize buffers on HEProject). 3. REN-808 rest: sky SPIR-V + `BuildSkyNoise3D` disk cache. 4. After Thema 212 merges, re-run `series.ps1` without `-NoValidation`. 5. Editor: `deviceWaitIdle` before `ImGui_ImplVulkan_Shutdown` (the shutdown validation messages).
