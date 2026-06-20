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
target_compile_definitions(RendererOpenGL PRIVATE HE_RENDERING_BUILD_DLL)

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
        foreach(stage scene.vert scene.frag scene_shadow.vert
                      postfx.vert postfx_tonemap.frag postfx_fxaa.frag
                      postfx_bloom_bright.frag postfx_bloom_blur.frag
                      sky.vert sky.frag
                      debug_line.vert debug_line.frag
                      ssao_pos.vert ssao_pos.frag ssao.frag ssao_blur.frag
                      skinned.vert
                      ui.vert ui.frag)
            set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${stage}")
            set(dst "${HE_SHADER_OUT_DIR}/${stage}.spv")
            if(GLSLC_EXECUTABLE MATCHES "glslangValidator")
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
    target_link_libraries(RendererMetal
        PUBLIC  "-framework Metal" "-framework QuartzCore" "-framework Foundation"
        PRIVATE glm::glm HorizonCore SDL3::SDL3)
    target_compile_definitions(RendererMetal PRIVATE HE_RENDERING_BUILD_DLL)
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
    )
    target_link_libraries(RendererD3D11 PRIVATE d3d11 dxgi d3dcompiler glm::glm HorizonCore)
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
    )
    target_link_libraries(RendererD3D12 PRIVATE d3d12 dxgi d3dcompiler glm::glm HorizonCore)
    target_compile_definitions(RendererD3D12 PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE HE_RENDERING_BUILD_DLL)
endif()

# ─── ViewportUI ────────────────────────────────────────────────────────────────
add_library(ViewportUI STATIC
    src/ViewportUI/UIManager.cpp)
target_include_directories(ViewportUI PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HE_CORE_INCLUDE}
)
target_link_libraries(ViewportUI PRIVATE glm::glm HorizonCore)

# ─── HorizonRendering DLL ──────────────────────────────────────────────────────
add_library(HorizonRendering SHARED
    src/RenderExtractor.cpp
    src/RenderWorld.cpp
    src/FrustumCuller.cpp
    src/RenderSorter.cpp
    src/RenderResourceManager.cpp
    src/GPUMemoryAllocator.cpp
    src/RenderGraph.cpp
    src/RenderPass.cpp
    src/CommandBuffer.cpp
    src/IRenderer.cpp
    src/RendererFactory.cpp)

target_include_directories(HorizonRendering
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/glad/include
)

set(HR_BACKENDS RendererOpenGL ViewportUI)
if(HE_VULKAN_ENABLED)
    list(APPEND HR_BACKENDS RendererVulkan)
endif()
if(WIN32)
    list(APPEND HR_BACKENDS RendererD3D11 RendererD3D12)
endif()
if(APPLE)
    list(APPEND HR_BACKENDS RendererMetal)
endif()

# HorizonScene is PRIVATE: only RenderExtractor.cpp touches the ECS —
# the dependency must never leak to consumers of HorizonRendering.
target_link_libraries(HorizonRendering
    PUBLIC  HorizonCore
    PRIVATE ${HR_BACKENDS} SDL3::SDL3 HorizonScene
)
target_compile_definitions(HorizonRendering
    PRIVATE HE_RENDERING_BUILD_DLL
    PUBLIC  HE_RENDERING_DLL
)
if(HE_VULKAN_ENABLED)
    target_compile_definitions(HorizonRendering PRIVATE HE_VULKAN_ENABLED)
endif()

he_deploy_dll(HorizonRendering)
