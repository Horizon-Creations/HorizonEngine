# Vulkan on macOS through MoltenVK (opt-in)

Branch `claude/hc-engine-settings-nodes`, no PR. Done on request, because it makes the
Vulkan backend something a Mac can build, start and shoot, not only syntax-check.

## What it is

`-DHE_ENABLE_MOLTENVK=ON` (macOS, default OFF) builds `RendererVulkan`, the ImGui Vulkan bridge
and the SPIR-V scene shaders on top of MoltenVK (Vulkan -> Metal).

```
brew install molten-vk                      # headers + libMoltenVK.dylib, glslc from brew shaderc
cmake -S . -B out/build/release -DHE_ENABLE_MOLTENVK=ON      # -DHE_MOLTENVK_ROOT=<prefix> to override
```

* `cmake/HEMoltenVK.cmake` finds the dylib and headers (`HE_MOLTENVK_ROOT`, then the LunarG
  SDK's `macOS/` layout, then Homebrew) and presets the cache entries `Vulkan_INCLUDE_DIR` /
  `Vulkan_LIBRARY` that `FindVulkan` reads. They must be cache entries: `HorizonEngineBundle.cmake`
  runs `find_package(Vulkan)` again at the root scope.
* `libMoltenVK` is linked directly (no Khronos loader, no ICD json). The deploy step
  (`bundle_native_deps`) already copies `libMoltenVK.dylib` next to `libHorizonEngine.dylib`.
* Without the option nothing changes: `HE_VULKAN_ENABLED` stays FALSE on macOS (checked by
  reconfiguring OFF and ON).

## Code changes (`VulkanRenderer.cpp`, `createInstance` / `createDevice`)

* SDL asks for `VK_KHR_portability_enumeration` on Apple whether or not the library has it. Linked
  straight to MoltenVK it has not, and `vkCreateInstance` failed with `VK_ERROR_EXTENSION_NOT_PRESENT`
  (-7). The request is now checked against the instance's own list; a listed extension also gets
  `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`. A native driver lists none of it: same path as before.
* A device that lists `VK_KHR_portability_subset` gets it enabled (mandatory per spec; spelled as a
  string, the macro is in `vulkan_beta.h`).
* `vkCheck` now prints the `VkResult`; one INFO line lists the instance extensions.

## Verified (2026-11-06, Apple M5, MoltenVK 1.4.1, headers 1.4.334)

* Builds: `RendererVulkan`, `VulkanShaders` (47 `.spv` via glslc), `HorizonGame`, `HorizonEditor`, `he_tests`.
* Exported game with `config.json` `CustomConfig GameBackend=Vulkan`, hidden window, 120 frames: rc 0,
  "device 0 of 1: Apple M5 (Vulkan 1.2.334)", swapchain/render pass/PostFX/sky/SSAO/skinned/UI pipelines created,
  0 errors. GI is the software path (no `VK_KHR_ray_query`); tessellation is on.
* Editor, Catania copy, `HE_DUMP_RHI=Vulkan` vs `Metal`, 30 frames: the Vulkan frame draws sky, clouds,
  water and the terrain mesh with shadows' direction right, **but the landscape is white** (see below).
* `ctest -R "vulkan|quality|game_build_config|material"`: 14/14 (includes `test_vulkan_shader_manager`).

## Found, not fixed

1. **Landscape layer-blend materials draw white on MoltenVK.** MoltenVK reports
   `maxPerStageDescriptorSamplers = 16` (`sampledImages 256`), with or without
   `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` (probed); `updateAfterBindSamplers` is 500000. The material
   set needs 17 (`VulkanRenderer.cpp:3033`, "heLandscapeWeights LEFT OUT (layer-blend materials draw
   built-in)"), so the fallback fires and the Catania terrain loses its layers. Options: fewer or shared
   samplers in the material set (separate image + sampler objects, the shared-sampler work of topic 324 is
   the same lever), or an update-after-bind layout for that binding. Any 16-sampler GPU shows this.
2. The `HE_DUMP_*` editor dump works for Vulkan, so a per-feature A/B against Metal is possible here now.

## Handoff

**Verified:** as above. **Not verified:** a visible (non-hidden) window and VSync on the Vulkan path,
`HE_GPU_DEBUG` validation layer (not installed), the editor's ImGui Vulkan bridge under interaction (only
one dumped frame), Texture Quality and the quality material lane on Vulkan (white terrain hides it),
pixel parity with Metal. **Decided:** direct link to MoltenVK, opt-in, no Khronos loader on macOS.
**Next steps, in order**
1. Fix finding 1 (17th sampler), then re-shoot Catania Vulkan vs Metal and read the quality lane on it
   (Hive topic 336 step 8 can then be partly checked on the Mac; the Windows-side checks stay open).
2. Try `HE_GPU_DEBUG=1` once the Vulkan SDK validation layer is installed (`brew install vulkan-validationlayers`).
3. Decide whether the Mac release package should ever carry MoltenVK (today: dev option only).
