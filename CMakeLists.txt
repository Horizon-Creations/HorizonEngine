cmake_minimum_required(VERSION 3.20)
project(HorizonEngine VERSION 0.0.1 LANGUAGES CXX)

# ─── Architecture guard ──────────────────────────────────────────────────────
# This engine is built and shipped as x64. If CMake is configured from an x86
# toolchain environment (e.g. the "x86 Native Tools" prompt / vcvars32), the
# linker pulls in 32-bit CRT and Windows Kit libs and fails with a wall of
# cryptic LNK2019/LNK1120 "unresolved external symbol" errors (operator new,
# memcpy, _DllMainCRTStartup, …) plus LNK4272 machine-type conflicts. Catch
# that here with an actionable message instead. Use configure_x64.bat or an
# "x64 Native Tools" prompt.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "HorizonEngine requires a 64-bit (x64) toolchain, but a 32-bit one was "
        "detected (CMAKE_SIZEOF_VOID_P=4). You likely configured from an x86 "
        "environment. Run configure_x64.bat or open an 'x64 Native Tools' "
        "command prompt and reconfigure with --fresh.")
endif()

# Objective-C++ is needed for the Metal backend and the ImGui Metal bridge.
# Must be enabled at the top level — enable_language() in subdirectories does
# not reliably propagate with all generators.
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

# ─── Windows headers ─────────────────────────────────────────────────────────
# <windows.h> (pulled in by the D3D11/D3D12 backends, SDL and the editor)
# defines min()/max() as function-like macros. They collide with
# std::min/std::max, std::numeric_limits<T>::max() and glm's min/max, producing
# cryptic C2589/C2059/C4003 "syntax error" cascades (e.g. in Math/AABB.h and
# EditorUI.cpp). NOMINMAX suppresses those macros engine-wide; WIN32_LEAN_AND_MEAN
# trims rarely-used Windows headers.
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ─── SDL3 ──────────────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.14
    GIT_SHALLOW    TRUE
)
set(SDL_SHARED  ON  CACHE BOOL "" FORCE)
set(SDL_STATIC  OFF CACHE BOOL "" FORCE)
set(SDL_TEST    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

# ─── GLM ───────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# ─── EnTT ─────────────────────────────────────────────────────────────────────
# Vendored as header-only in src/HE_Scene/vendor/entt — no FetchContent needed.

# ─── nlohmann/json ────────────────────────────────────────────────────────────
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests      OFF CACHE BOOL "" FORCE)
set(JSON_Install         OFF CACHE BOOL "" FORCE)
set(JSON_MultipleHeaders OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ─── Lua 5.4 ──────────────────────────────────────────────────────────────────
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/walterschell/Lua.git
    GIT_TAG        v5.4.7
    GIT_SHALLOW    TRUE
)
set(LUA_BUILD_SHARED    OFF CACHE BOOL "" FORCE)
set(LUA_ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lua)

# ─── Jolt Physics ─────────────────────────────────────────────────────────────
FetchContent_Declare(
    JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.5.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build
)
set(TARGET_HELLO_WORLD      OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES          OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER           OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS       OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL          OFF CACHE BOOL "" FORCE)
# Match the engine's /MD runtime — Jolt defaults to /MT which causes LNK2038
# "RuntimeLibrary conflict MTd_StaticDebug vs MDd_DynamicDebug" at link time.
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(JoltPhysics)

# ─── Recast Navigation ────────────────────────────────────────────────────────
FetchContent_Declare(
    recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)
set(RECASTNAVIGATION_DEMO     OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS    OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(recastnavigation)

# ─── LZ4 (hpak compression) ───────────────────────────────────────────────────
# Use system lz4 if available (Homebrew on macOS, package manager on Linux/Windows).
# Falls back gracefully: if not found, hpak entries can still be stored uncompressed.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LZ4 QUIET liblz4)
endif()
if(NOT LZ4_FOUND)
    find_path(LZ4_INCLUDE_DIRS lz4.h
        PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(LZ4_LIBRARIES NAMES lz4
        PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(LZ4_INCLUDE_DIRS AND LZ4_LIBRARIES)
        set(LZ4_FOUND TRUE)
    endif()
endif()
if(LZ4_FOUND)
    message(STATUS "HorizonEngine: LZ4 found (system) — hpak compression enabled")
    add_library(lz4_import INTERFACE)
    target_include_directories(lz4_import INTERFACE ${LZ4_INCLUDE_DIRS})
    target_link_libraries(lz4_import INTERFACE ${LZ4_LIBRARIES})
    add_library(lz4::lz4 ALIAS lz4_import)
    set(HE_HAVE_LZ4 TRUE)
else()
    # System LZ4 not found — build it from source so compression is always available
    # and gets distributed with the engine (statically linked into HorizonCore, no
    # extra DLL to ship). We intentionally skip lz4's own CMakeLists.txt (which
    # requires cmake_minimum_required < 3.5, removed in CMake 3.27) and create our
    # own minimal target directly from lz4's lib/ sources.
    message(STATUS "HorizonEngine: LZ4 not found on system — fetching v1.9.4 from source")
    FetchContent_Declare(
        lz4
        GIT_REPOSITORY https://github.com/lz4/lz4.git
        GIT_TAG        v1.9.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(lz4)
    if(NOT lz4_POPULATED)
        # FetchContent_Populate downloads without calling add_subdirectory, which
        # lets us bypass lz4's old cmake_minimum_required.
        # CMP0169 (CMake >= 3.28) deprecates this form; suppress the warning.
        if(POLICY CMP0169)
            cmake_policy(SET CMP0169 OLD)
        endif()
        FetchContent_Populate(lz4)
        if(POLICY CMP0169)
            cmake_policy(SET CMP0169 NEW)
        endif()
    endif()
    # LZ4 is written in C; enable the C compiler for this target only.
    enable_language(C)
    add_library(lz4_static STATIC
        "${lz4_SOURCE_DIR}/lib/lz4.c"
        "${lz4_SOURCE_DIR}/lib/lz4hc.c"
    )
    target_include_directories(lz4_static PUBLIC "${lz4_SOURCE_DIR}/lib")
    set_target_properties(lz4_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(NOT TARGET lz4::lz4)
        add_library(lz4::lz4 ALIAS lz4_static)
    endif()
    set(HE_HAVE_LZ4 TRUE)
endif()

# ─── zstd (hpak ship codec + dictionary training) ─────────────────────────────
# Use system zstd if available (Homebrew on macOS, package manager elsewhere);
# otherwise build a minimal static lib from source. Falls back gracefully: without
# zstd, hpak still supports Store + LZ4 codecs.
if(PkgConfig_FOUND)
    pkg_check_modules(ZSTD QUIET libzstd)
endif()
if(NOT ZSTD_FOUND)
    find_path(ZSTD_INCLUDE_DIRS zstd.h
        PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(ZSTD_LIBRARIES NAMES zstd
        PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(ZSTD_INCLUDE_DIRS AND ZSTD_LIBRARIES)
        set(ZSTD_FOUND TRUE)
    endif()
endif()
if(ZSTD_FOUND)
    message(STATUS "HorizonEngine: zstd found (system) — hpak zstd codec enabled")
    add_library(zstd_import INTERFACE)
    target_include_directories(zstd_import INTERFACE ${ZSTD_INCLUDE_DIRS})
    target_link_libraries(zstd_import INTERFACE ${ZSTD_LIBRARIES})
    add_library(zstd::zstd ALIAS zstd_import)
    set(HE_HAVE_ZSTD TRUE)
else()
    message(STATUS "HorizonEngine: zstd not found on system — fetching v1.5.6 from source")
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG        v1.5.6
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(zstd)
    if(NOT zstd_POPULATED)
        if(POLICY CMP0169)
            cmake_policy(SET CMP0169 OLD)
        endif()
        FetchContent_Populate(zstd)
    endif()
    enable_language(C)
    file(GLOB ZSTD_SRC
        "${zstd_SOURCE_DIR}/lib/common/*.c"
        "${zstd_SOURCE_DIR}/lib/compress/*.c"
        "${zstd_SOURCE_DIR}/lib/decompress/*.c"
        "${zstd_SOURCE_DIR}/lib/dictBuilder/*.c")
    add_library(zstd_static STATIC ${ZSTD_SRC})
    target_include_directories(zstd_static PUBLIC "${zstd_SOURCE_DIR}/lib")
    target_compile_definitions(zstd_static PRIVATE ZSTD_DISABLE_ASM=1)
    set_target_properties(zstd_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(NOT TARGET zstd::zstd)
        add_library(zstd::zstd ALIAS zstd_static)
    endif()
    set(HE_HAVE_ZSTD TRUE)
endif()

# ─── OpenSSL (hpak AES-256-GCM encryption) ────────────────────────────────────
# Encryption of shipped assets is obfuscation against casual ripping, NOT a
# security boundary (the key ships with the game). Optional: without OpenSSL the
# hpak encrypt path is simply unavailable (assets pack unencrypted).
if(NOT OPENSSL_ROOT_DIR AND EXISTS "/opt/homebrew/opt/openssl@3")
    set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
endif()
find_package(OpenSSL QUIET COMPONENTS Crypto)
if(OPENSSL_FOUND)
    message(STATUS "HorizonEngine: OpenSSL found — hpak AES-256-GCM enabled")
    set(HE_HAVE_OPENSSL TRUE)
else()
    message(STATUS "HorizonEngine: OpenSSL not found — hpak encryption disabled")
endif()

# ─── Tracy (optional profiling) ────────────────────────────────────────────────
# Enable with:  cmake -DHE_ENABLE_TRACY=ON ...
# Default OFF so production/CI builds have zero overhead.
option(HE_ENABLE_TRACY "Enable Tracy frame profiler instrumentation" OFF)
if(HE_ENABLE_TRACY)
    FetchContent_Declare(
        Tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG        v0.11.1
        GIT_SHALLOW    TRUE
    )
    set(TRACY_ENABLE   ON  CACHE BOOL "" FORCE)
    set(TRACY_NO_EXIT  ON  CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(Tracy)
    message(STATUS "HorizonEngine: Tracy profiling ENABLED")
else()
    message(STATUS "HorizonEngine: Tracy profiling disabled (pass -DHE_ENABLE_TRACY=ON to enable)")
endif()

# ── In-engine profiler (EngineProfiler scopes / editor profiling window) ──────
# ON for development/editor builds; the HE_PROFILE_SCOPE_N macros expand to the
# EngineProfiler RAII timers. Turn OFF for a shipping GAME build
# (cmake -DHE_PROFILING=OFF ...) → the scope macros become no-ops, so the shipped
# game carries ZERO profiler overhead (not even the per-scope recording check).
# Defined globally (before add_subdirectory) so the shared engine libs that hold
# the system scopes (Terrain, render passes, …) are instrumented for the editor.
option(HE_PROFILING "In-engine EngineProfiler scopes (editor/dev)" ON)
if(HE_PROFILING)
    add_compile_definitions(HE_PROFILING_ENABLED)
    message(STATUS "HorizonEngine: in-engine profiler ENABLED (pass -DHE_PROFILING=OFF for a zero-overhead shipping game)")
else()
    message(STATUS "HorizonEngine: in-engine profiler compiled OUT (zero overhead)")
endif()

# ─── Deploy directories ────────────────────────────────────────────────────────
if(WIN32)
    set(DEPLOY_DIR    "C:/Users/conno/source/repos/HorizonEngineBuild")
else()
    set(DEPLOY_DIR    "${CMAKE_SOURCE_DIR}/out/deploy")
endif()
set(DEPLOY_GAME   "${DEPLOY_DIR}/Game")
set(DEPLOY_EDITOR "${DEPLOY_DIR}/Editor")

# ─── Deploy helper ─────────────────────────────────────────────────────────────
# Usage: he_deploy_dll(<target>)
# Adds POST_BUILD steps that copy the DLL to DEPLOY_EDITOR and optionally
# DEPLOY_GAME every time the target is rebuilt.
# he_deploy_dll copies the DLL only to the Editor deploy directory.
# Game DLLs are compiled separately (without HE_EDITOR) and deployed
# by the HorizonGame post-build step.
function(he_deploy_dll target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_EDITOR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${target}> "${DEPLOY_EDITOR}"
        VERBATIM
    )
endfunction()

# ─── Version header (codename + version → generated HorizonVersion.h) ────────────
# Sky-themed release codename (see docs/version-codenames.md). Change this one line
# per release; it surfaces in the editor title/Project Hub and drives the DMG theme.
set(HE_VERSION_CODENAME "First Light" CACHE STRING "Sky-themed codename for this release")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/HorizonVersion.h.in"
    "${CMAKE_BINARY_DIR}/generated/HorizonVersion.h"
    @ONLY)
# Global so every engine lib + the editor can #include "HorizonVersion.h".
include_directories("${CMAKE_BINARY_DIR}/generated")

add_subdirectory(src/HE_Core)
add_subdirectory(src/HE_Scene)
add_subdirectory(src/HE_Rendering)
add_subdirectory(src/HE_Tools)
add_subdirectory(src/HE_Game)
add_subdirectory(src/HE_Editor)

# ─── Tests ─────────────────────────────────────────────────────────────────────
option(HE_BUILD_TESTS "Build the doctest unit tests" ON)
if(HE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
