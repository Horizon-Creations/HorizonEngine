cmake_minimum_required(VERSION 3.20)
project(HorizonRendering VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── GLAD
add_library(glad STATIC
    glad/src/glad.c)
target_include_directories(glad PUBLIC
    glad/include
)

set(HE_CORE_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../HE_Core/include)

# ─── Shared cross-backend material shader layer ────────────────────────────────
# Backend-agnostic: MaterialAsset custom GLSL → per-backend source via he::shaderc.
# Every backend links this and builds its own pipeline object from the shared source.
# ALWAYS built: he_shadercompiler is either the real cross-compiler or the stub that
# answers every compile with a failure (root CMakeLists). The difference is
# HE_HAVE_SHADERC, and it means one thing only — "may cross-compile at runtime".
# The material path itself, precompiled variants included, is in every build.
add_library(he_materialshader STATIC
    src/material/MaterialShaderLibrary.cpp
    include/material/MaterialShaderLibrary.h)
target_include_directories(he_materialshader PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HE_CORE_INCLUDE})
target_link_libraries(he_materialshader PUBLIC HorizonCore PRIVATE he_shadercompiler)
target_compile_features(he_materialshader PUBLIC cxx_std_20)
if(HE_HAVE_SHADERC)
    target_compile_definitions(he_materialshader PUBLIC HE_HAVE_SHADERC=1)
endif()

# ─── OpenGL Backend ────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

add_library(RendererOpenGL STATIC
    src/Backends/OpenGL/OpenGLRenderer.cpp
    src/Backends/OpenGL/OpenGLShaderManager.cpp
    include/Backends/OpenGL/OpenGLShaderManager.h)
target_include_directories(RendererOpenGL PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/glad/include
    ${HE_CORE_INCLUDE}
)
target_link_libraries(RendererOpenGL PRIVATE glad OpenGL::GL glm::glm HorizonCore)
# Shared cross-backend material shader layer (per-material GL programs). Propagates
# HE_HAVE_SHADERC=1 so the material path compiles in when the cross-compiler was fetched.
if(TARGET he_materialshader)
    target_link_libraries(RendererOpenGL PRIVATE he_materialshader he_shadercompiler)
endif()
target_compile_definitions(RendererOpenGL PRIVATE HE_RENDERING_BUILD_DLL)

# ─── Software Backend (UI only, no GPU) ────────────────────────────────────────
# docs/he-apps-plan.md Block G: the renderer an application with "Advanced Shader
# Effects" switched off ships with. No glad, no OpenGL::GL, no shader compiler —
# that absence IS the feature, and it is why this is its own library rather than
# a mode of another one.
#
# Split in two: SoftwareRaster is quads-in-pixels-out with no SDL and no
# IRenderer, so it is unit-testable; SoftwareRenderer is the shell that gets the
# quads and puts them in a window.
add_library(RendererSoftware STATIC
    src/Backends/Software/SoftwareRaster.cpp
    src/Backends/Software/SoftwareRenderer.cpp
    include/Backends/Software/SoftwareRaster.h
    include/Backends/Software/SoftwareRenderer.h)
target_include_directories(RendererSoftware PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HE_CORE_INCLUDE}
)
target_link_libraries(RendererSoftware PUBLIC HorizonCore PRIVATE glm::glm SDL3::SDL3)
target_compile_features(RendererSoftware PUBLIC cxx_std_20)
target_compile_definitions(RendererSoftware PRIVATE HE_RENDERING_BUILD_DLL)

# ─── Vulkan Backend ────────────────────────────────────────────────────────────
# CACHE INTERNAL so the resolved value is visible to sibling subdirectories
# (notably src/HE_Editor, which links RendererVulkan and compiles the ImGui
# Vulkan backend). A plain set() here would stay local to this directory scope,
# leaving HE_Editor with an empty HE_VULKAN_ENABLED → unresolved VulkanRenderer
# symbols at link time.
set(HE_VULKAN_ENABLED FALSE CACHE INTERNAL "Vulkan backend available")
if(DEFINED ENV{VULKAN_SDK})
    set(Vulkan_INCLUDE_DIR "$ENV{VULKAN_SDK}/Include")
    if(WIN32)
        set(Vulkan_LIBRARY "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
    else()
        set(Vulkan_LIBRARY "$ENV{VULKAN_SDK}/lib/libvulkan.so")
    endif()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(HE_VULKAN_ENABLED TRUE CACHE INTERNAL "Vulkan backend available")
    endif()
else()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(HE_VULKAN_ENABLED TRUE CACHE INTERNAL "Vulkan backend available")
    endif()
endif()

if(HE_VULKAN_ENABLED)
    add_library(RendererVulkan STATIC
        src/Backends/Vulkan/VulkanRenderer.cpp
        src/Backends/Vulkan/VulkanShaderManager.cpp
        include/Backends/Vulkan/VulkanShaderManager.h)
    target_include_directories(RendererVulkan PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HE_CORE_INCLUDE}
        ${Vulkan_INCLUDE_DIRS}
    )
    target_link_libraries(RendererVulkan PUBLIC Vulkan::Vulkan PRIVATE glm::glm HorizonCore)
    # A4: shared material-graph shader layer (GLSL→SPIR-V via glslang/SPIRV-Cross) so the
    # Vulkan backend can build per-material pipelines from node-graph materials.
    if(TARGET he_materialshader)
        target_link_libraries(RendererVulkan PRIVATE he_materialshader he_shadercompiler)
    endif()
    target_compile_definitions(RendererVulkan PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE HE_RENDERING_BUILD_DLL)
    message(STATUS "HorizonRendering: Vulkan backend enabled")

    # ── Compile the scene GLSL to SPIR-V (glslc) ─────────────────────────────
    # The Vulkan backend loads <exe dir>/Shaders/*.spv at runtime. We compile
    # them here when a shader compiler is available (it ships with the Vulkan
    # SDK, the same SDK that gates this whole backend). Apps must deploy the
    # resulting Shaders/ folder next to their executable (alongside the engine
    # DLLs); the editor/game deploy steps copy it from ${CMAKE_BINARY_DIR}/Shaders.
    find_program(GLSLC_EXECUTABLE NAMES glslc glslangValidator
                 HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
    if(GLSLC_EXECUTABLE)
        set(HE_SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/Shaders")
        file(MAKE_DIRECTORY "${HE_SHADER_OUT_DIR}")
        set(HE_VULKAN_SPV "")
        foreach(stage scene.vert scene_instanced.vert scene.frag scene_shadow.vert
                      postfx.vert postfx_tonemap.frag postfx_fxaa.frag
                      postfx_smaa.frag postfx_aa_blit.frag
                      postfx_bloom_bright.frag postfx_bloom_blur.frag
                      sky.vert sky.frag
                      debug_line.vert debug_line.frag
                      ssao_pos.vert ssao_pos.frag ssao.frag ssao_blur.frag
                      gi_gbuf.vert gi_gbuf.frag gi_temporal.frag gi_blur.frag
                      gi_shadow.comp gi_probe.comp
                      gi_shadow_hw.comp gi_probe_hw.comp
                      skinned.vert
                      ui.vert ui.frag)
            set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${stage}")
            set(dst "${HE_SHADER_OUT_DIR}/${stage}.spv")
            # The *_hw.comp kernels use GL_EXT_ray_query (SPIR-V 1.4+) and need an
            # explicit Vulkan-1.2 target; the default target (SPIR-V 1.0) rejects them.
            if(stage MATCHES "_hw\\.comp$")
                if(GLSLC_EXECUTABLE MATCHES "glslangValidator")
                    add_custom_command(OUTPUT "${dst}"
                        COMMAND ${GLSLC_EXECUTABLE} -V --target-env vulkan1.2 "${src}" -o "${dst}"
                        DEPENDS "${src}" VERBATIM)
                else()
                    add_custom_command(OUTPUT "${dst}"
                        COMMAND ${GLSLC_EXECUTABLE} --target-env=vulkan1.2 "${src}" -o "${dst}"
                        DEPENDS "${src}" VERBATIM)
                endif()
            elseif(GLSLC_EXECUTABLE MATCHES "glslangValidator")
                add_custom_command(OUTPUT "${dst}"
                    COMMAND ${GLSLC_EXECUTABLE} -V "${src}" -o "${dst}"
                    DEPENDS "${src}" VERBATIM)
            else()
                add_custom_command(OUTPUT "${dst}"
                    COMMAND ${GLSLC_EXECUTABLE} "${src}" -o "${dst}"
                    DEPENDS "${src}" VERBATIM)
            endif()
            list(APPEND HE_VULKAN_SPV "${dst}")
        endforeach()
        add_custom_target(VulkanShaders DEPENDS ${HE_VULKAN_SPV})
        add_dependencies(RendererVulkan VulkanShaders)
    else()
        message(WARNING "HorizonRendering: glslc not found — Vulkan scene shaders not compiled")
    endif()
else()
    message(STATUS "HorizonRendering: Vulkan backend disabled (SDK not found)")
endif()

# ─── Metal Backend (macOS) ─────────────────────────────────────────────────────
if(APPLE)
    add_library(RendererMetal STATIC
        src/Backends/Metal/MetalRenderer.mm
        src/Backends/Metal/MetalShaderManager.mm
        include/Backends/Metal/MetalRenderer.h
        include/Backends/Metal/MetalShaderManager.h)
    target_include_directories(RendererMetal PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HE_CORE_INCLUDE}
    )
    set_source_files_properties(
        src/Backends/Metal/MetalRenderer.mm
        src/Backends/Metal/MetalShaderManager.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # MetalFX (docs/anti-aliasing-plan.md A5): the OS temporal scaler. Weak-linked
    # so a build still runs on a system whose SDK predates it — the backend asks
    # MTLFXTemporalScalerDescriptor at runtime and falls back to its own TAA.
    target_link_libraries(RendererMetal
        PUBLIC  "-framework Metal" "-framework QuartzCore" "-framework Foundation"
                "-weak_framework MetalFX"
        PRIVATE glm::glm HorizonCore SDL3::SDL3)
    target_compile_definitions(RendererMetal PRIVATE HE_RENDERING_BUILD_DLL)
    # Cross-backend shader codegen (material system M1). The shared material shader
    # layer (he_materialshader) turns MaterialAsset GLSL into MSL; RendererMetal builds
    # the pipeline objects. Both pull HE_HAVE_SHADERC=1 so the material path compiles in.
    if(TARGET he_materialshader)
        target_link_libraries(RendererMetal PRIVATE he_materialshader he_shadercompiler)
    endif()
    message(STATUS "HorizonRendering: Metal backend enabled")
endif()

# ─── D3D11 Backend ─────────────────────────────────────────────────────────────
if(WIN32)
    add_library(RendererD3D11 STATIC
        src/Backends/D3D11/D3D11Renderer.cpp
        src/Backends/D3D11/D3D11ShaderManager.cpp
        include/Backends/D3D11/D3D11ShaderManager.h)
    target_include_directories(RendererD3D11 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HE_CORE_INCLUDE}
        # src/ so both D3D backends can reach src/Backends/D3D_Shared/HlslSources.h,
        # the HLSL they share verbatim. PRIVATE: nothing outside the backend needs it.
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(RendererD3D11 PRIVATE d3d11 dxgi d3dcompiler glm::glm HorizonCore)
    # A4: shared material-graph shader layer (GLSL→HLSL via glslang/SPIRV-Cross) so the
    # D3D11 backend can build per-material VS/PS from node-graph materials. Propagates
    # HE_HAVE_SHADERC=1 (mirrors RendererVulkan/D3D12).
    if(TARGET he_materialshader)
        target_link_libraries(RendererD3D11 PRIVATE he_materialshader he_shadercompiler)
    endif()
    target_compile_definitions(RendererD3D11 PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE HE_RENDERING_BUILD_DLL)
endif()

# ─── D3D12 Backend ─────────────────────────────────────────────────────────────
if(WIN32)
    add_library(RendererD3D12 STATIC
        src/Backends/D3D12/D3D12Renderer.cpp
        src/Backends/D3D12/D3D12ShaderManager.cpp
        include/Backends/D3D12/D3D12ShaderManager.h)
    target_include_directories(RendererD3D12 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HE_CORE_INCLUDE}
        # src/ so both D3D backends can reach src/Backends/D3D_Shared/HlslSources.h,
        # the HLSL they share verbatim. PRIVATE: nothing outside the backend needs it.
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(RendererD3D12 PRIVATE d3d12 dxgi d3dcompiler glm::glm HorizonCore)
    # A4: shared material-graph shader layer (GLSL→HLSL via glslang/SPIRV-Cross) so the
    # D3D12 backend can build per-material PSOs from node-graph materials. Propagates
    # HE_HAVE_SHADERC=1 (mirrors what RendererVulkan added).
    if(TARGET he_materialshader)
        target_link_libraries(RendererD3D12 PRIVATE he_materialshader he_shadercompiler)
    endif()
    target_compile_definitions(RendererD3D12 PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE HE_RENDERING_BUILD_DLL)

    # ── GI hardware ray-tracing kernels (DXR 1.1 inline RayQuery, SM 6.5) ────
    # FXC (D3DCompile) cannot target SM 6, so the two RayQuery compute kernels
    # are compiled OFFLINE by dxc into <build>/Shaders/*.cso — the same folder
    # the Vulkan .spv land in; the editor deploy step copies it next to the exe
    # and the D3D12 backend loads <exe dir>/Shaders/gi_*_hw.cso at runtime.
    # dxc ships with recent Windows 10/11 SDKs (and the Vulkan SDK). If it is
    # missing, only warn: the runtime checks file existence and silently keeps
    # the embedded SM 5.0 software-ray-tracing kernels (no functional break).
    set(HE_D3D12_GI_SHADERS FALSE CACHE INTERNAL "D3D12 DXR GI kernels compiled")
    find_program(DXC_EXECUTABLE NAMES dxc dxc.exe
                 HINTS "$ENV{WindowsSdkVerBinPath}x64" "$ENV{WindowsSdkVerBinPath}/x64"
                       "$ENV{WindowsSdkVerBinPath}"
                       "$ENV{WindowsSdkDir}bin/x64"    "$ENV{WindowsSdkDir}/bin/x64"
                       "$ENV{VULKAN_SDK}/Bin"          "$ENV{VULKAN_SDK}/bin")
    if(DXC_EXECUTABLE)
        set(HE_D3D12_SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/Shaders")
        file(MAKE_DIRECTORY "${HE_D3D12_SHADER_OUT_DIR}")
        set(HE_D3D12_GI_CSO "")
        foreach(kernel gi_shadow_hw gi_probe_hw)
            set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${kernel}.hlsl")
            set(dst "${HE_D3D12_SHADER_OUT_DIR}/${kernel}.cso")
            add_custom_command(OUTPUT "${dst}"
                COMMAND ${DXC_EXECUTABLE} -T cs_6_5 -E main -Fo "${dst}" "${src}"
                DEPENDS "${src}" VERBATIM)
            list(APPEND HE_D3D12_GI_CSO "${dst}")
        endforeach()
        add_custom_target(D3D12GiShaders DEPENDS ${HE_D3D12_GI_CSO})
        add_dependencies(RendererD3D12 D3D12GiShaders)
        set(HE_D3D12_GI_SHADERS TRUE CACHE INTERNAL "D3D12 DXR GI kernels compiled")
        message(STATUS "HorizonRendering: D3D12 DXR GI kernels enabled (dxc: ${DXC_EXECUTABLE})")
    else()
        message(WARNING "HorizonRendering: dxc not found — D3D12 GI hardware ray-tracing "
                        "kernels not compiled (runtime falls back to software GI)")
    endif()
endif()

# ─── HorizonRendering DLL ──────────────────────────────────────────────────────
add_library(HorizonRendering SHARED
    src/RenderExtractor.cpp
    src/RenderWorld.cpp
    src/FrustumCuller.cpp
    src/RenderSorter.cpp
    src/ScenePick.cpp
    # Cross-backend shared renderer helpers (audit 1a). The backend static libs
    # only reuse this target's PUBLIC include path — they do not link it — but
    # they are linked INTO HorizonRendering (and, for the editor, alongside it),
    # so these definitions resolve at the final link exactly like RenderGraph /
    # CommandBuffer already do.
    src/SsaoKernel.cpp
    src/SkyNoise3D.cpp
    src/SkyFrameParams.cpp
    src/LightPacking.cpp
    src/RenderResourceManager.cpp
    src/GPUMemoryAllocator.cpp
    src/RenderGraph.cpp
    src/RenderPass.cpp
    src/CommandBuffer.cpp
    src/GiBvh.cpp
    src/GiInstanceSurface.cpp
    src/RendererFactory.cpp)

target_include_directories(HorizonRendering
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/glad/include
)

# ─── Which backends this runtime carries (docs/he-apps-plan.md A3b) ───────────
# All backend libraries are still BUILT above — the editor links several of them
# by name, and a target that exists costs nothing until something links it. What
# the runtime flavour decides is which ones end up INSIDE HorizonRendering:
#
#   game          every backend the platform has            (as before)
#   app-advanced  the platform's forward renderer, alone    (Metal on macOS, GL elsewhere)
#   app-basic     RendererSoftware, alone                   (no GPU stack at all)
#
# Each linked backend also gets a HE_BACKEND_* define on HorizonRendering, and
# RendererFactory switches on those rather than on the platform macros: asking
# "am I on Windows" answered the wrong question the moment a Windows runtime
# could deliberately carry only OpenGL.
if(HE_RUNTIME_FLAVOR STREQUAL "app-basic")
    set(HR_BACKENDS RendererSoftware)
    set(HR_BACKEND_DEFS HE_BACKEND_SOFTWARE)
elseif(HE_RUNTIME_FLAVOR STREQUAL "app-advanced")
    if(APPLE)
        set(HR_BACKENDS RendererMetal)
        set(HR_BACKEND_DEFS HE_BACKEND_METAL)
    else()
        set(HR_BACKENDS RendererOpenGL)
        set(HR_BACKEND_DEFS HE_BACKEND_OPENGL)
    endif()
else()
    set(HR_BACKENDS RendererOpenGL RendererSoftware)
    set(HR_BACKEND_DEFS HE_BACKEND_OPENGL HE_BACKEND_SOFTWARE)
    if(HE_VULKAN_ENABLED)
        list(APPEND HR_BACKENDS RendererVulkan)
        list(APPEND HR_BACKEND_DEFS HE_BACKEND_VULKAN)
    endif()
    if(WIN32)
        list(APPEND HR_BACKENDS RendererD3D11 RendererD3D12)
        list(APPEND HR_BACKEND_DEFS HE_BACKEND_D3D11 HE_BACKEND_D3D12)
    endif()
    if(APPLE)
        list(APPEND HR_BACKENDS RendererMetal)
        list(APPEND HR_BACKEND_DEFS HE_BACKEND_METAL)
    endif()
endif()
message(STATUS "HorizonRendering: backends linked (${HE_RUNTIME_FLAVOR}): ${HR_BACKENDS}")

# HorizonScene is PRIVATE: only RenderExtractor.cpp touches the ECS —
# the dependency must never leak to consumers of HorizonRendering.
target_link_libraries(HorizonRendering
    PUBLIC  HorizonCore
    PRIVATE ${HR_BACKENDS} SDL3::SDL3 HorizonScene
)
target_compile_definitions(HorizonRendering
    PRIVATE HE_RENDERING_BUILD_DLL ${HR_BACKEND_DEFS}
            HE_RUNTIME_FLAVOR_NAME="${HE_RUNTIME_FLAVOR}"
    PUBLIC  HE_RENDERING_DLL
)
if(HE_VULKAN_ENABLED)
    target_compile_definitions(HorizonRendering PRIVATE HE_VULKAN_ENABLED)
endif()

he_deploy_dll(HorizonRendering)

# ─── Embedded-shader validation ────────────────────────────────────────────────
# The D3D and GL backends hold most of their shaders as string literals compiled at
# RUNTIME (D3DCompile / glCompileShader), so the C++ build never type-checks them.
# Worse, both backends answer a compile failure by logging once and disabling the
# feature for the session — a broken shader reads as "the toggle does nothing".
# That is how a non-compiling GI kernel silently killed the whole GI stack on both
# D3D backends, DXR included (docs/backend-parity-plan.md §1.1).
#
# This target extracts those strings, reassembles them exactly as the call sites do
# and runs fxc / glslangValidator over them (~3 s for 75 shaders). The script skips
# whichever half has no compiler installed, so a checkout without the Windows or
# Vulkan SDK still configures and builds.
option(HE_VALIDATE_SHADERS "Compile-check the runtime-compiled shader strings" ON)
if(HE_VALIDATE_SHADERS AND Python3_Interpreter_FOUND)
    add_custom_target(HeValidateShaders ALL
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/validate_embedded_shaders.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Validating runtime-compiled shader strings (fxc / glslangValidator)"
        VERBATIM)
    message(STATUS "HorizonRendering: embedded-shader validation enabled")
elseif(HE_VALIDATE_SHADERS)
    message(STATUS "HorizonRendering: no Python3 interpreter — embedded-shader validation skipped")
endif()
