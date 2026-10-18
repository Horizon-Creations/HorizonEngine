# ─── Engine SDK (Thema 211 step 5) ────────────────────────────────────────────
# What a game project needs to be compiled OUTSIDE this tree: the engine
# libraries of THIS build, their public headers, the HorizonGame sources and a
# CMake package (find_package(HorizonEngine)) whose imported targets carry the
# same compile definitions the in-tree consumers get, the build profile above all.
# Installed as the component "HorizonSDK":
#
#   cmake --build <tree> --target HorizonSDK            -> ${HE_SDK_DIR}
#   cmake --install <tree> --component HorizonSDK --prefix <dir>
#
# Layout (relocatable, nothing below refers back to the tree):
#   include/                    engine include + vendor dirs, glm, nlohmann, SDL3,
#                               HorizonVersion.h — one flat root, so <sdk> is also a
#                               valid HE_HCGEN_SDK root for the HorizonCode codegen
#   lib/                        engine + SDL3 shared libs (Windows: import libs)
#   bin/                        Windows: the DLLs
#   lib/cmake/HorizonEngine/    HorizonEngineConfig(+Version).cmake, HorizonEngineGame.cmake
#   share/HorizonEngine/game/   the HorizonGame sources horizon_add_game() compiles
#   share/HorizonEngine/template/  a minimal game project (CMakeLists.txt)
#   share/HorizonEngine/Shaders/   runtime shader binaries, when this tree has any
#
# One SDK per tree and per profile: the libraries are compiled with the profile
# (HE_BUILD_PROFILE), so a release SDK comes from a release tree. Imported targets
# rather than install(EXPORT): HorizonCore's PUBLIC dependencies (glm, SDL3,
# nlohmann_json, lua_static) belong to no export set, and their include dirs are
# absolute paths into the fetch cache.
#
# Included at the END of the root CMakeLists.txt: it reads the targets' final
# interface definitions.

include_guard(GLOBAL)
include(CMakePackageConfigHelpers)

set(HE_SDK_DIR "${DEPLOY_DIR}/SDK-${HE_BUILD_PROFILE}" CACHE PATH
    "Where the HorizonSDK target installs the engine SDK (never <editor>/SDK: the editor's POST_BUILD wipes that)")

set(_he_sdk_libs HorizonCore HorizonNet HorizonScene HorizonRendering)
foreach(_t IN LISTS _he_sdk_libs)
    if(NOT TARGET ${_t})
        message(FATAL_ERROR "HorizonSDK: engine library ${_t} does not exist in this tree")
    endif()
endforeach()
get_target_property(_he_sdl SDL3::SDL3 ALIASED_TARGET)
if(NOT _he_sdl)
    set(_he_sdl SDL3::SDL3)
endif()
FetchContent_GetProperties(SDL3)
if(HE_ENABLE_TRACY)
    message(WARNING "HorizonSDK: HE_ENABLE_TRACY is on, but the SDK ships no Tracy "
                    "headers; a game built against it will not compile")
endif()

set(_he_sdk_component HorizonSDK)
set(_he_sdk_cmake_dir "lib/cmake/HorizonEngine")

# ── Libraries ────────────────────────────────────────────────────────────────
# Linux: DT_RUNPATH is not transitive, so each installed .so must find its
# siblings itself (the build tree gets $ORIGIN from CMAKE_BUILD_RPATH; the
# install step would strip it). macOS resolves @rpath through the loading
# executable, Windows looks beside the exe.
if(UNIX AND NOT APPLE)
    set_property(TARGET ${_he_sdk_libs} PROPERTY INSTALL_RPATH "$ORIGIN")
endif()
install(TARGETS ${_he_sdk_libs} ${_he_sdl}
    COMPONENT ${_he_sdk_component}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib NAMELINK_COMPONENT ${_he_sdk_component}
    ARCHIVE DESTINATION lib)

# ── Headers ──────────────────────────────────────────────────────────────────
# Exactly the include dirs the libraries publish, flattened (no two of them
# share a top-level name). Headers only: vendor/ also holds bc7enc's .cpp.
foreach(_dir
        src/HE_Core/include src/HE_Core/vendor
        src/HE_Net/include
        src/HE_Scene/include src/HE_Scene/vendor
        src/HE_Rendering/include)
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/${_dir}/"
        DESTINATION include COMPONENT ${_he_sdk_component}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.inl")
endforeach()
install(FILES "${CMAKE_BINARY_DIR}/generated/HorizonVersion.h"
    DESTINATION include COMPONENT ${_he_sdk_component})
install(DIRECTORY "${glm_SOURCE_DIR}/glm"
    DESTINATION include COMPONENT ${_he_sdk_component}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.inl")
install(DIRECTORY "${nlohmann_json_SOURCE_DIR}/single_include/nlohmann"
    DESTINATION include COMPONENT ${_he_sdk_component})
install(DIRECTORY "${sdl3_SOURCE_DIR}/include/SDL3"
    DESTINATION include COMPONENT ${_he_sdk_component})
# SDL's build generates SDL_revision.h and puts its dir first on the include
# path; installed after the source headers so it replaces theirs.
install(DIRECTORY "${sdl3_BINARY_DIR}/include-revision/SDL3"
    DESTINATION include COMPONENT ${_he_sdk_component})

# ── HorizonGame sources ──────────────────────────────────────────────────────
# Shipped as source, not as a library: the game's own dev features
# (HE_FEATURE_*) and the compiled-in configuration (CompiledBuildConfig.cpp +
# HEGameBuildConfig.h) are decided when the game is compiled.
file(GLOB _he_sdk_game_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/HE_Game/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/HE_Game/src/*.h"
    "${CMAKE_SOURCE_DIR}/src/HE_Game/src/*.mm")
install(FILES ${_he_sdk_game_sources}
    DESTINATION share/HorizonEngine/game COMPONENT ${_he_sdk_component})
install(FILES "${CMAKE_SOURCE_DIR}/cmake/sdk/template/CMakeLists.txt"
    DESTINATION share/HorizonEngine/template COMPONENT ${_he_sdk_component})
if(HE_VULKAN_ENABLED OR HE_D3D12_GI_SHADERS)
    install(DIRECTORY "${CMAKE_BINARY_DIR}/Shaders"
        DESTINATION share/HorizonEngine COMPONENT ${_he_sdk_component})
endif()

# ── CMake package ────────────────────────────────────────────────────────────
# Per imported target: the file names (generator expressions, so Windows DLL +
# import lib and SDL3's versioned name come out right) and the target's own
# interface definitions (HE_*_DLL, the profile), read here, after every
# target_compile_definitions has run. The directory-level definitions
# (add_compile_definitions in the root) are in no target's interface, so they
# are added by name.
set(HE_SDK_GLOBAL_DEFINITIONS "")
if(WIN32)
    list(APPEND HE_SDK_GLOBAL_DEFINITIONS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(HE_PROFILING)
    list(APPEND HE_SDK_GLOBAL_DEFINITIONS HE_PROFILING_ENABLED)
endif()
foreach(_t IN LISTS _he_sdk_libs)
    get_target_property(_defs ${_t} INTERFACE_COMPILE_DEFINITIONS)
    if(NOT _defs)
        set(_defs "")
    endif()
    if(_defs MATCHES "\\$<")
        message(FATAL_ERROR "HorizonSDK: ${_t} has a generator expression in its "
                            "interface definitions (${_defs}); the SDK config cannot carry it")
    endif()
    set(HE_SDK_DEFS_${_t} "${_defs}")
endforeach()
# The HE_FEATURE_* defaults per profile live in Platform/BuildProfile.h (shipped
# in include/); an explicit FEATURES override on a library is in its list above.
set(HE_SDK_VULKAN_ENABLED OFF)
if(HE_VULKAN_ENABLED)
    set(HE_SDK_VULKAN_ENABLED ON)
endif()
set(HE_SDK_SHADERS OFF)
if(HE_VULKAN_ENABLED OR HE_D3D12_GI_SHADERS)
    set(HE_SDK_SHADERS ON)
endif()
set(HE_SDK_SDL_TARGET ${_he_sdl})
# Where the loadable files go: DLLs to bin/ beside their import libs in lib/.
if(WIN32)
    set(HE_SDK_RUNTIME_SUBDIR bin)
    set(HE_SDK_IMPLIB TRUE)
else()
    set(HE_SDK_RUNTIME_SUBDIR lib)
    set(HE_SDK_IMPLIB FALSE)
endif()

set(_he_sdk_gen "${CMAKE_BINARY_DIR}/HorizonSDK")
configure_file("${CMAKE_SOURCE_DIR}/cmake/sdk/HorizonEngineConfig.cmake.in"
               "${_he_sdk_gen}/HorizonEngineConfig.cmake.genex" @ONLY)
file(GENERATE
    OUTPUT "${_he_sdk_gen}/$<CONFIG>/HorizonEngineConfig.cmake"
    INPUT  "${_he_sdk_gen}/HorizonEngineConfig.cmake.genex")
# ExactVersion: the libraries have no stable ABI between engine versions.
write_basic_package_version_file("${_he_sdk_gen}/HorizonEngineConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES
        "${_he_sdk_gen}/$<CONFIG>/HorizonEngineConfig.cmake"
        "${_he_sdk_gen}/HorizonEngineConfigVersion.cmake"
        "${CMAKE_SOURCE_DIR}/cmake/sdk/HorizonEngineGame.cmake"
    DESTINATION ${_he_sdk_cmake_dir} COMPONENT ${_he_sdk_component})

# ── The target ───────────────────────────────────────────────────────────────
# Not ALL: an engine build should not pay for a copy nobody asked for. The
# install script copies the files the dependencies below have just built.
add_custom_target(HorizonSDK
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --config $<CONFIG>
            --component ${_he_sdk_component} --prefix "${HE_SDK_DIR}"
    COMMENT "Installing the engine SDK (${HE_BUILD_PROFILE}) into ${HE_SDK_DIR}"
    VERBATIM)
add_dependencies(HorizonSDK ${_he_sdk_libs} ${_he_sdl})
message(STATUS "HorizonEngine: SDK target HorizonSDK -> ${HE_SDK_DIR}")
