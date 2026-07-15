cmake_minimum_required(VERSION 3.20)
project(HorizonEngine VERSION 0.2.0 LANGUAGES CXX)

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

# ─── Position-independent code ───────────────────────────────────────────────
# The engine modules (HorizonCore/Scene/Rendering) are shared libraries. On Linux,
# any static library linked into a shared object must be built with -fPIC, or ld
# aborts with "relocation R_X86_64_PC32 against symbol '…' can not be used when
# making a shared object; recompile with -fPIC" (hit first via lua_static, then
# astcenc). Enable PIC globally so every FetchContent static dependency (lua_static,
# astcenc, Jolt, Recast, glm, …) is position-independent. This is a no-op on macOS
# (PIC is the default there) and ignored by MSVC on Windows, so it is safe on all
# platforms and subsumes the per-target settings on lz4_static/zstd_static below.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
    # pkg-config's LZ4_LIBRARIES is the BARE name ("lz4") — without the -L dir the
    # linker only finds it when Homebrew's lib dir happens to be on the default path
    # (true locally, NOT on CI runners → "ld: library 'lz4' not found"). Prefer the
    # absolute paths pkg-config also provides; fall back to name + link dirs.
    if(LZ4_LINK_LIBRARIES)
        target_link_libraries(lz4_import INTERFACE ${LZ4_LINK_LIBRARIES})
    else()
        if(LZ4_LIBRARY_DIRS)
            target_link_directories(lz4_import INTERFACE ${LZ4_LIBRARY_DIRS})
        endif()
        target_link_libraries(lz4_import INTERFACE ${LZ4_LIBRARIES})
    endif()
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
    # Same bare-name pitfall as LZ4 above — prefer absolute paths from pkg-config.
    if(ZSTD_LINK_LIBRARIES)
        target_link_libraries(zstd_import INTERFACE ${ZSTD_LINK_LIBRARIES})
    else()
        if(ZSTD_LIBRARY_DIRS)
            target_link_directories(zstd_import INTERFACE ${ZSTD_LIBRARY_DIRS})
        endif()
        target_link_libraries(zstd_import INTERFACE ${ZSTD_LIBRARIES})
    endif()
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

# ─── Crypto backend (hpak AES-256-GCM encryption) ─────────────────────────────
# Encryption of shipped assets is obfuscation against casual ripping, NOT a
# security boundary (the key ships with the game). The primary backend is system
# OpenSSL; if it isn't installed we fetch mbedTLS from source so AES-256-GCM is
# ALWAYS available and encryption is never silently disabled. Both feed the same
# backend-agnostic wrapper (src/HE_Core/src/Hpak/Aes256Gcm.cpp), selected at
# compile time via HE_HAVE_OPENSSL / HE_HAVE_MBEDTLS.
# Prefer the fetched, statically-linked mbedTLS over system OpenSSL for a fully
# self-contained build (no external libcrypto DLL on Windows). Set for the shipping /
# CI package build (-DHE_PREFER_MBEDTLS=ON); default keeps the faster system OpenSSL.
option(HE_PREFER_MBEDTLS "Use static mbedTLS for hpak crypto instead of system OpenSSL (self-contained)" OFF)
if(NOT OPENSSL_ROOT_DIR AND EXISTS "/opt/homebrew/opt/openssl@3")
    set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
endif()
if(NOT HE_PREFER_MBEDTLS)
    find_package(OpenSSL QUIET COMPONENTS Crypto)
endif()
if(OPENSSL_FOUND)
    message(STATUS "HorizonEngine: OpenSSL found (system) — hpak AES-256-GCM enabled")
    set(HE_HAVE_OPENSSL TRUE)
else()
    # No system OpenSSL — build mbedTLS instead (CMake-native, unlike OpenSSL's
    # Perl/nasm build). We fetch the RELEASE TARBALL, not the git tag: the tarball
    # ships the pre-generated sources so GEN_FILES can be OFF and no Python/Jinja2
    # codegen toolchain is needed. Only libmbedcrypto is used (GCM + CTR-DRBG).
    if(HE_PREFER_MBEDTLS)
        message(STATUS "HorizonEngine: HE_PREFER_MBEDTLS — fetching static mbedTLS for AES-256-GCM (self-contained)")
    else()
        message(STATUS "HorizonEngine: OpenSSL not found — fetching mbedTLS for AES-256-GCM")
    endif()
    FetchContent_Declare(
        mbedtls
        URL      https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
        URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
    )
    set(ENABLE_PROGRAMS        OFF CACHE BOOL "" FORCE) # no sample apps
    set(ENABLE_TESTING         OFF CACHE BOOL "" FORCE) # no test suites (needs framework submodule)
    set(GEN_FILES              OFF CACHE BOOL "" FORCE) # tarball ships generated sources
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE) # don't -Werror mbedTLS on new MSVC
    FetchContent_MakeAvailable(mbedtls)
    if(TARGET mbedcrypto)
        set(HE_HAVE_MBEDTLS TRUE)
        message(STATUS "HorizonEngine: mbedTLS fetched — hpak AES-256-GCM enabled (mbedcrypto)")
    else()
        message(STATUS "HorizonEngine: mbedTLS target not found — hpak encryption disabled")
    endif()
endif()

# Backend-agnostic crypto usage requirements. Consumers (HorizonCore, hpak_packer)
# link `he_crypto` and get the right library plus HE_HAVE_CRYPTO and the concrete
# backend macro. Linking it is a no-op when neither backend resolved, in which case
# encryption is compiled out (Hpak::cryptoAvailable() returns false).
add_library(he_crypto INTERFACE)
if(HE_HAVE_OPENSSL)
    target_link_libraries(he_crypto INTERFACE OpenSSL::Crypto)
    target_compile_definitions(he_crypto INTERFACE HE_HAVE_CRYPTO HE_HAVE_OPENSSL)
elseif(HE_HAVE_MBEDTLS)
    target_link_libraries(he_crypto INTERFACE mbedcrypto)
    target_compile_definitions(he_crypto INTERFACE HE_HAVE_CRYPTO HE_HAVE_MBEDTLS)
endif()

# ─── Python (optional: CPython embedding for Python gameplay scripting) ───────
# Gameplay scripts can be written in Python (in addition to Lua). Dev builds link
# libpython; without it, the Python script backend is compiled out and only Lua
# is available. Shipping needs a bundled interpreter (see docs/scripting design).
# Interpreter is requested alongside Development.Embed so the shipping bundle step
# (src/HE_Game) can locate the stdlib (Python3_STDLIB) and run the interpreter to zip it.
find_package(Python3 QUIET COMPONENTS Interpreter Development.Embed)
if(Python3_Development.Embed_FOUND)
    message(STATUS "HorizonEngine: Python3 found (${Python3_VERSION}) — Python scripting enabled")
    set(HE_HAVE_PYTHON TRUE)
else()
    message(STATUS "HorizonEngine: Python3 embed libs not found — Python scripting disabled (Lua only)")
endif()

# ─── astcenc (optional: ASTC texture compression cook) ─────────────────────────
# The pack-time texture cook can transcode RGBA8 → ASTC for GPUs that sample it
# (Apple-Silicon Metal). Without it, textures stay RGBA8 (+ baked mipmaps).
# Disable with -DHE_ENABLE_ASTC=OFF if the encoder fails to configure.
option(HE_ENABLE_ASTC "Fetch astc-encoder for ASTC texture cooking" ON)
if(HE_ENABLE_ASTC)
    FetchContent_Declare(
        astcenc
        GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git
        GIT_TAG        5.3.0
        GIT_SHALLOW    TRUE
    )
    set(ASTCENC_CLI             OFF CACHE BOOL "" FORCE)
    set(ASTCENC_UNITTEST        OFF CACHE BOOL "" FORCE)
    set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE) # single-arch (native) lib
    set(ASTCENC_ISA_NATIVE      ON  CACHE BOOL "" FORCE)
    set(ASTCENC_INVARIANCE      OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(astcenc)
    if(TARGET astcenc-native-static)
        set(HE_HAVE_ASTCENC TRUE)
        message(STATUS "HorizonEngine: astcenc found — ASTC texture cooking enabled")
    else()
        message(STATUS "HorizonEngine: astcenc target not found — ASTC cooking disabled")
    endif()
endif()

# ─── Shader cross-compiler: glslang + SPIRV-Cross (Material-System M0) ──────────
# Canonical GLSL → glslang → SPIR-V → SPIRV-Cross → MSL / HLSL / GLSL(-ES).
# The backbone for the cross-backend material system (docs/material-system-design.md).
# Disable with -DHE_ENABLE_SHADERC=OFF (skips the fetch + the he_shader_spike tool).
option(HE_ENABLE_SHADERC "Fetch glslang + SPIRV-Cross for cross-backend shader codegen" ON)
if(HE_ENABLE_SHADERC)
    # glslang (GLSL → SPIR-V). ENABLE_OPT=OFF avoids the SPIRV-Tools optimizer submodule.
    FetchContent_Declare(
        glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG        15.4.0
        GIT_SHALLOW    TRUE
    )
    set(ENABLE_OPT               OFF CACHE BOOL "" FORCE) # no SPIRV-Tools optimizer dep
    set(GLSLANG_TESTS            OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL   OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES  OFF CACHE BOOL "" FORCE) # skip the standalone exes
    set(ENABLE_SPVREMAPPER       OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL              OFF CACHE BOOL "" FORCE) # we author canonical GLSL, not HLSL
    set(BUILD_TESTING            OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glslang)

    # SPIRV-Cross (SPIR-V → MSL/HLSL/GLSL). Static libs, no CLI, no tests.
    FetchContent_Declare(
        spirv_cross
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG        vulkan-sdk-1.4.313.0
        GIT_SHALLOW    TRUE
    )
    set(SPIRV_CROSS_CLI          OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_GLSL  ON  CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_MSL   ON  CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_HLSL  ON  CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_CPP   OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SHARED       OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_STATIC       ON  CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(spirv_cross)

    if(TARGET glslang AND TARGET SPIRV AND TARGET spirv-cross-msl)
        set(HE_HAVE_SHADERC TRUE)
        message(STATUS "HorizonEngine: glslang + SPIRV-Cross found — shader cross-compile enabled")

        # Reusable in-process cross-compiler lib. glslang + SPIRV-Cross are linked
        # PRIVATE so consumers only need src/HE_Tools/ShaderCompiler/ShaderCompiler.h.
        add_library(he_shadercompiler STATIC
            ${CMAKE_SOURCE_DIR}/src/HE_Tools/ShaderCompiler/ShaderCompiler.cpp)
        target_include_directories(he_shadercompiler PUBLIC
            ${CMAKE_SOURCE_DIR}/src/HE_Tools/ShaderCompiler)
        target_link_libraries(he_shadercompiler PRIVATE
            glslang SPIRV glslang-default-resource-limits
            spirv-cross-core spirv-cross-glsl spirv-cross-msl spirv-cross-hlsl)
        target_compile_features(he_shadercompiler PUBLIC cxx_std_20)
        target_compile_definitions(he_shadercompiler PUBLIC HE_HAVE_SHADERC=1)

        # M0 spike: prove GLSL → SPIR-V → MSL/HLSL/GLSL end-to-end.
        add_executable(he_shader_spike ${CMAKE_SOURCE_DIR}/src/HE_Tools/ShaderCompiler/spike_main.cpp)
        target_link_libraries(he_shader_spike PRIVATE he_shadercompiler)
    else()
        message(STATUS "HorizonEngine: glslang/SPIRV-Cross targets not found — shader cross-compile disabled")
    endif()
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
# Overridable with -DDEPLOY_DIR=... (CI packaging points it at the workspace; the
# default is the local dev deploy layout).
if(WIN32)
    set(_default_deploy_dir "C:/Users/conno/source/repos/HorizonEngineBuild")
else()
    set(_default_deploy_dir "${CMAKE_SOURCE_DIR}/out/deploy")
endif()
set(DEPLOY_DIR "${_default_deploy_dir}" CACHE PATH "Where the editor/game POST_BUILD deploys")
set(DEPLOY_GAME   "${DEPLOY_DIR}/Game")
set(DEPLOY_EDITOR "${DEPLOY_DIR}/Editor")

# ─── Linux: find bundled .so next to the executable ($ORIGIN) ───────────────────
# The editor/game POST_BUILD copies the engine .so (HorizonCore/Scene/Rendering/
# Tools) flat next to the binary. For a downloaded package to run, each binary must
# look beside itself for its libraries. DT_RUNPATH is NOT transitive, so $ORIGIN has
# to be on the executable AND every shared lib. Appending to CMAKE_BUILD_RPATH adds
# $ORIGIN to every target configured below (it is added to — not replacing — the
# auto build-tree rpath, so the build-tree binaries and ctest still resolve libs).
# macOS uses @executable_path rpaths set per-target + install_name_tool in
# package_macos.sh; Windows has no rpath concept.
if(UNIX AND NOT APPLE)
    set(CMAKE_BUILD_RPATH "$ORIGIN")
endif()

# Reusable CPython-runtime bundling (he_bundle_python) for the shipped game + editor.
include("${CMAKE_SOURCE_DIR}/cmake/BundlePython.cmake")

# ─── MSVC C++ runtime bundling ─────────────────────────────────────────────────
# The engine ships multiple DLLs (HorizonCore/Scene/Rendering/Tools, SDL3) that must
# share one CRT heap, so it links the dynamic CRT (/MD) — meaning a clean Windows PC
# would otherwise need the VC++ redistributable. Static-linking (/MT) is NOT an option
# here: each DLL would get its own CRT heap and a malloc-in-one/free-in-another across
# a DLL boundary crashes. So we bundle the redist DLLs (vcruntime140.dll, msvcp140.dll,
# …) next to the executable instead — self-contained, no redist install.
if(WIN32)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP ON) # we copy manually — no install() rule
    include(InstallRequiredSystemLibraries)
endif()
# he_bundle_crt(<target> <dest>): POST_BUILD-copy the MSVC runtime DLLs into <dest>.
function(he_bundle_crt target dest)
    if(WIN32 AND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${dest}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} "${dest}"
            COMMENT "Bundling the MSVC C++ runtime into ${dest}"
            VERBATIM)
    endif()
endfunction()

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
# FORCE so editing this line always wins over a value already stored in the CMake
# cache — without it, a reconfigure keeps the old cached codename (the version from
# project() updates, but the codename would silently stay stale).
set(HE_VERSION_CODENAME "Sunrise" CACHE STRING "Sky-themed codename for this release" FORCE)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/HorizonVersion.h.in"
    "${CMAKE_BINARY_DIR}/generated/HorizonVersion.h"
    @ONLY)
# Global so every engine lib + the editor can #include "HorizonVersion.h".
include_directories("${CMAKE_BINARY_DIR}/generated")

add_subdirectory(src/HE_Core)
add_subdirectory(src/HE_Net)
add_subdirectory(src/HE_Scene)
add_subdirectory(src/HE_Rendering)
add_subdirectory(src/HE_Tools)
add_subdirectory(src/HE_Game)
add_subdirectory(src/HE_Editor)

# Building the editor always (re)builds + deploys the game runtime: the editor's
# Build > Export ships those binaries, and a stale/missing out/deploy/Game
# silently produced data-only, unrunnable exports.
if(TARGET HorizonEditor AND TARGET HorizonGame)
    add_dependencies(HorizonEditor HorizonGame)
endif()

# ─── Tests ─────────────────────────────────────────────────────────────────────
option(HE_BUILD_TESTS "Build the doctest unit tests" ON)
if(HE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
