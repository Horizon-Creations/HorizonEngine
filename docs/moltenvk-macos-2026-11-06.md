# Vulkan on macOS through MoltenVK (on by default)

Branch `claude/hc-engine-settings-nodes`, no PR. Done on request, because it makes the
Vulkan backend something a Mac can build, start and shoot, not only syntax-check.

## What it is

`HE_ENABLE_MOLTENVK` (macOS, **default ON** since 2026-11-06, was opt-in) builds `RendererVulkan`, the ImGui
Vulkan bridge and the SPIR-V scene shaders on top of MoltenVK (Vulkan -> Metal). Metal stays the default
backend; Vulkan is selected per project / game (`GameBackend`).

```
brew install molten-vk shaderc              # headers + libMoltenVK.dylib, glslc from shaderc
cmake -S . -B out/build/release             # -DHE_MOLTENVK_ROOT=<prefix> to override, -DHE_ENABLE_MOLTENVK=OFF to leave out
```

* **Not installed is fine:** the module only prints "MoltenVK: not found ... Vulkan backend off on macOS"
  (a warning only when an explicit `HE_MOLTENVK_ROOT` finds nothing). A missing `glslc` leaves the SPIR-V shaders out
  with the existing warning.
* **Shipped with the package:** `libMoltenVK.dylib` is a link-time dependency of `libHorizonEngine.dylib`, so
  `scripts/bundle_native_deps.sh` copies and relocates it (`@rpath`) like every other bundled dylib; it was already
  in the exported game dir (`HorizonEngine/libMoltenVK.dylib`, minos 11.0). CI installs `molten-vk shaderc` before
  "Configure (macOS)" (`brew install ... || warning`, so a failed brew call does not fail the job).
* Not verified: a CI run of the macOS job with the new step, and the DMG with MoltenVK inside (`package_macos.sh`,
  `otool -L` check there) - both need CI.

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
* Editor, Catania copy, `HE_DUMP_RHI=Vulkan` vs `Metal`, 30 frames, same camera: first the landscape was
  white (below), after the fix Vulkan matches Metal (mean abs pixel difference 50.1 -> 4.4 of 255; the rest is
  sky/cloud and water, not the terrain). `HE_DUMP_QUALITYPRESET=Low` changes the Vulkan frame by 7.4 of 255
  against 7.7 on Metal, so the material quality lane and the other Low settings reach Vulkan.
* Full `ctest`: 365/365.

## The 17th sampler: shared samplers (fixed)

MoltenVK reports `maxPerStageDescriptorSamplers = 16` (`sampledImages 256`), with or without
`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` (probed; `updateAfterBindSamplers` is 500000). The material set
needed 17 combined image samplers, so `heLandscapeWeights` was left out ("layer-blend materials draw
built-in") and the Catania terrain drew white. Any 16-sampler GPU would have shown this.

Fix, the way an Unreal material's "Shared: Clamp" sampler source works: texture and sampler are separate
descriptors, so the limit that bites (samplers, 16) only counts the distinct samplers, not the textures
(images, 128-256).

* The `Backend::SpirV` flavour of the lighting preamble (`HE_VK_SHARED_SAMPLERS`, `injectPreamble`) declares the
  eleven fixed textures (bindings 10-13, 15-18, 31-33: GI masks, CSM, local shadows, sky cube, AO, DDGI atlases,
  SSR, GI reflections, cloud shadow) as `texture2D` / `texture2DArray` / `textureCube` plus two samplers,
  `heSampLinear` (34: linear, clamp-to-edge) and `heSampShadow` (35: nearest, clamp-to-white-border), and
  `#define heAO sampler2D(heAO_t, heSampLinear)` etc., so no call site changes. Metal, GL and D3D read the
  same preamble without the define.
* `VulkanMaterialLayout.h`: `DescKind::SampledImage` / `Sampler`; the fixed rows are images, 34/35 are the
  samplers (both set tables). Fragment stage of the material set: 8 samplers (heTex0, heTexP0..3,
  heLandscapeWeights, the two shared ones) and 17 images; resolve set 6 and 15. `landscapeWeightsFit` now
  checks images and samplers separately (a device at the image minimum of 16 still gets the fallback).
* `VulkanRenderer.cpp`: pools and layouts take the two new types, the fixed writes are SAMPLED_IMAGE, two
  SAMPLER writes (SSAO sampler, shadow sampler) per set. heCloudShadow is back (a white image).
* heTex0 / heTexP0..3 / heLandscapeWeights stay combined: their sampler is the texture's own (wrap mode).
  A per-texture "Sampler Source" option on the Texture Sample node (Texture / Shared Wrap / Shared Clamp)
  would be the user-facing half of this; not built.

Verified: Catania on Vulkan before vs after the change: mean pixel difference 0.10 of 255 (High) and 0.09
(Low preset), 0.001% of pixels over 40; against Metal still 4.4. Game run 240 frames, 0 errors. Tests:
`test_material_graph` / `test_engine_materials` / `test_material_tessellation` reflect separate images and
samplers now (the reflection loops fail on any shader that declares a fixed texture combined).

## Handoff

**Verified:** as above. **Not verified:** a visible (non-hidden) window and VSync on the Vulkan path,
`HE_GPU_DEBUG` validation layer (not installed), the editor's ImGui Vulkan bridge under interaction (only
one dumped frame), Texture Quality on Vulkan, per-feature pixel parity with Metal (only the whole frame was compared). **Decided:** direct link to MoltenVK, opt-in, no Khronos loader on macOS.
**Done since (2026-11-06, same branch):** MoltenVK on by default (above); Texture Quality on Vulkan; shadow presets
confirmed on Vulkan; material quality alone A/B against Metal; the governor run on Vulkan. Numbers and files:
`docs/engine-quality-settings-2026-11-05.md` section 10.
**Next steps, in order**
1. CI: let the macOS job run once with the `Install MoltenVK + shaderc` step; check `HE_VULKAN_ENABLED` in its CMakeCache
   and `otool -L` of the packaged editor (libMoltenVK `@rpath`-relative, inside the DMG's Frameworks).
2. Hive topic 336 step 8 (Vulkan): a Windows/Linux driver run, a visible window with VSync, validation layers
   (`brew install vulkan-validationlayers`, then `HE_GPU_DEBUG=1`).
3. Per-node "Sampler Source" (Texture / Shared Wrap / Shared Clamp) on the Texture Sample node, only if wanted.
