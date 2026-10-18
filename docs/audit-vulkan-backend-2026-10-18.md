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
* dGPU (NN-WS03): open a large scene with and without `HE_VK_UPLOAD_SYNC=1` and
  compare the upload line plus first-frame time (F9 profiler). This is pending, see
  below.

## Handoff

- **Topic / step:** 215, step 1 of 2
- **Branch / commit:** `claude/audit-optimierung-vulkan-backend-stufe-3` @ 4266f16b (pushed: yes)
- **Done:** Asset uploads on Vulkan no longer stall the queue per asset. Static/skeletal meshes are device-local and sub-allocated.
- **Files touched:** `src/HE_Rendering/include/Backends/Vulkan/VulkanUploadAlloc.h` (ring + free list, Vulkan-free);
  `VulkanRenderer.h:668` (upload-path members, `GpuMesh::valloc/ialloc`, `GpuSkeletalMesh::*Alloc`);
  `VulkanRenderer.cpp:7284-7650` (pool, batch, wrappers, stats), `:7731`/`:7823` (texture uploads), `:7970` (albedo set first),
  `:10437` (ImGui icons), `:392` (teardown); `tests/test_vulkan_upload_alloc.cpp`; `scripts/he_vk_imagetests.py` (sync variants, upload line).
- **Verified:** `-fsyntax-only` of VulkanRenderer.cpp against MoltenVK 1.4.1 headers (recipe from the topic lesson) passes with rc 0 and no new `-Wall -Wextra` warnings in the touched lines; the negative control (misspelled member) fails. `he_tests --source-file='*test_vulkan_upload_alloc.cpp'`: 6/6, 46736 assertions. A mutated header (ring overrun, wrong merge) makes 3/6 fail. Full `ctest -j4` (Release, out/build/release): 309/310. Red is `test_hpak` (`test_hpak.cpp:1188`, `scanContentDirectory() == 2` reads 6), also when run alone. The diff touches neither ContentManager nor that test, and he_tests does not link the Vulkan renderer. Not checked against the base commit.
- **Not verified:** the renderer has not run on any Vulkan device here (the Mac has no loader). Runtime and validation come from CI run https://github.com/Horizon-Creations/HorizonEngine/actions/runs/39139994324 (lavapipe images + Windows MSVC compile), still running when this was written. No dGPU timing.
- **Decisions taken:** memory sub-allocation, not one shared buffer with `vertexOffset`/`firstIndex` (bind sites untouched); each batch has its own fence instead of riding on the frame serial (one-shot submits never bump that serial); images stay dedicated; empty 64 MB blocks are freed at once (simpler; costs re-allocation churn on level reloads).
- **Decisions suggested, not taken:** one vertex/index mega-buffer (REN-803 as the audit wrote it); pool image memory too; move the remaining one-shot waits onto the batch.
- **Pending:** step 2 (`VkPipelineCache`, REN-808); dGPU load-time A/B on NN-WS03; `AUDIT.md` Stufe-3 Vulkan box once step 2 is in.
- **Traps:** every new `vkQueueSubmit`/`vkDeviceWaitIdle` must use `submitGraphics`/`deviceWaitIdle`, otherwise it can run ahead of a pending copy. Create the view and stage the data before recording a copy, never destroy after. Submitters outside the file (imgui_impl_vulkan platform windows) do not go through the wrapper.
- **Next steps:** 1. Read the CI run: lavapipe `clustered`/`ui_image` `sync vs …` lines must be ok, with no new validation messages; the Windows job must compile RendererVulkan. 2. Step 2: `VkPipelineCache` created in `createDevice`'s neighbourhood, passed to all 33 `vkCreateGraphicsPipelines`, persisted next to the exe. 3. On NN-WS03: large scene, `HE_VK_UPLOAD_SYNC=0/1`, compare the shutdown upload line and first-frame ms.
