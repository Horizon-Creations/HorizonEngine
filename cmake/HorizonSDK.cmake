# ─── Engine SDK (Thema 211 steps 5 and 9) ─────────────────────────────────────
# What a game project needs to be compiled and linked OUTSIDE this tree, and
# nothing more: the engine libraries of THIS build, the headers a compiler
# reaches from the engine's public headers and the HorizonGame sources, those
# sources, and a CMake package (find_package(HorizonEngine)) whose imported
# targets carry the compile definitions the in-tree consumers get, the build
# profile above all. No repository, no engine sources, no runtime data.
# Installed as the component "HorizonSDK":
#
#   cmake --build <tree> --target HorizonSDK            -> ${HE_ENGINE_SDK_DIR}
#   cmake --install <tree> --component HorizonSDK --prefix <dir>
#
# ONE package for both profiles. The libraries are compiled with the profile
# (HE_BUILD_PROFILE: the HE_FEATURE_* sites inside Core/Scene are #if'd), so a
# dev tree and a release tree each install their own lib set into the same
# prefix; the consumer picks one (find_package COMPONENTS, or HE_BUILD_PROFILE):
#
#   include/                       shared by both profiles: engine include dirs,
#                                  entt, glm, nlohmann, SDL3, HorizonVersion.h
#   lib/<profile>/                 engine + SDL3 shared libs (Windows: import libs)
#   bin/<profile>/                 Windows: the DLLs
#   lib/cmake/HorizonEngine/       HorizonEngineConfig(+Version).cmake (profile-neutral),
#                                  HorizonEngineTargets-<profile>.cmake (one per lib set),
#                                  HorizonEngineGame.cmake, the header stamps (below)
#   share/HorizonEngine/game/      the HorizonGame sources horizon_add_game() compiles
#   share/HorizonEngine/template/  the game project the editor's export configures
#
# Both lib sets must come from the same engine sources, since they share one
# include/. Every install stamps the headers it leaves behind
# (HorizonEngineHeaders.txt) and the ones its lib set was installed with
# (HorizonEngineHeaders-<profile>.txt); the package refuses a lib set whose stamp
# differs, so a release set from an older build next to newer headers is "not
# found" instead of a game that crashes at its first call.
#
# Imported targets rather than install(EXPORT): HorizonCore's PUBLIC dependencies
# (glm, SDL3, nlohmann_json, lua_static) belong to no export set, and their
# include dirs are absolute paths into the fetch cache.
#
# Included at the END of the root CMakeLists.txt: it reads the targets' final
# interface definitions.

include_guard(GLOBAL)
include(CMakePackageConfigHelpers)

# Not HE_SDK_DIR any more (that one held SDK-<profile>, one folder per profile):
# a renamed variable keeps an older cache from installing into the old folder.
set(HE_ENGINE_SDK_DIR "${DEPLOY_DIR}/EngineSDK" CACHE PATH
    "Where the HorizonSDK target installs this tree's lib set of the engine SDK (never <editor>/SDK: the editor's POST_BUILD wipes that)")

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
set(_he_sdk_profile "${HE_BUILD_PROFILE}")

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
    RUNTIME DESTINATION bin/${_he_sdk_profile}
    LIBRARY DESTINATION lib/${_he_sdk_profile} NAMELINK_SKIP
    ARCHIVE DESTINATION lib/${_he_sdk_profile})

# ── Headers ──────────────────────────────────────────────────────────────────
# The include dirs the libraries publish, flattened (no two of them share a
# top-level name), and of the third-party dirs only what a public header or
# the game sources include. Not shipped, because nothing the compiler reaches
# from there includes them: HE_Core/vendor (stb, fonts, bc7enc — the libraries'
# own implementation detail), miniaudio.h, the loose entt.hpp/fwd.hpp copies,
# and SDL's test, OpenGL and EGL headers (SDL.h does not include them; the GL
# backend is inside HorizonRendering). tests/sdk_example checks that no shipped
# header names one of them.
foreach(_dir src/HE_Core/include src/HE_Net/include src/HE_Scene/include src/HE_Rendering/include)
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/${_dir}/"
        DESTINATION include COMPONENT ${_he_sdk_component}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.inl")
endforeach()
install(DIRECTORY "${CMAKE_SOURCE_DIR}/src/HE_Scene/vendor/entt"
    DESTINATION include COMPONENT ${_he_sdk_component}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
install(FILES "${CMAKE_BINARY_DIR}/generated/HorizonVersion.h"
    DESTINATION include COMPONENT ${_he_sdk_component})
install(DIRECTORY "${glm_SOURCE_DIR}/glm"
    DESTINATION include COMPONENT ${_he_sdk_component}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.inl")
install(DIRECTORY "${nlohmann_json_SOURCE_DIR}/single_include/nlohmann"
    DESTINATION include COMPONENT ${_he_sdk_component})
install(DIRECTORY "${sdl3_SOURCE_DIR}/include/SDL3"
    DESTINATION include COMPONENT ${_he_sdk_component}
    PATTERN "SDL_test*.h" EXCLUDE
    PATTERN "SDL_opengl*.h" EXCLUDE
    PATTERN "SDL_egl.h" EXCLUDE)
# SDL's build generates SDL_revision.h and puts its dir first on the include
# path; installed after the source headers so it replaces theirs.
install(DIRECTORY "${sdl3_BINARY_DIR}/include-revision/SDL3"
    DESTINATION include COMPONENT ${_he_sdk_component})

# ── HorizonGame sources ──────────────────────────────────────────────────────
# Shipped as source, not as a library: the game's own dev features
# (HE_FEATURE_*) and the compiled-in configuration (CompiledBuildConfig.cpp +
# HEGameBuildConfig.h) are decided when the game is compiled. Runtime data
# (Shaders/, the Python plugin) is not here: the export folder of the prebuilt
# runtime already has it, and the SDK-built game is swapped into that folder.
file(GLOB _he_sdk_game_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/HE_Game/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/HE_Game/src/*.h"
    "${CMAKE_SOURCE_DIR}/src/HE_Game/src/*.mm")
install(FILES ${_he_sdk_game_sources}
    DESTINATION share/HorizonEngine/game COMPONENT ${_he_sdk_component})
install(FILES "${CMAKE_SOURCE_DIR}/cmake/sdk/template/CMakeLists.txt"
    DESTINATION share/HorizonEngine/template COMPONENT ${_he_sdk_component})

# ── CMake package ────────────────────────────────────────────────────────────
# Per imported target: the file names (generator expressions, so Windows DLL +
# import lib and SDL3's versioned name come out right) and the target's own
# interface definitions (HE_*_DLL, the profile), read here, after every
# target_compile_definitions has run. The directory-level definitions
# (add_compile_definitions in the root) are in no target's interface, so they
# are added by name. All of it is per lib set: HorizonEngineTargets-<profile>.cmake.
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
set(HE_SDK_SDL_TARGET ${_he_sdl})
# Where the loadable files go: DLLs to bin/<profile> beside their import libs in lib/<profile>.
if(WIN32)
    set(HE_SDK_RUNTIME_SUBDIR bin)
    set(HE_SDK_IMPLIB TRUE)
else()
    set(HE_SDK_RUNTIME_SUBDIR lib)
    set(HE_SDK_IMPLIB FALSE)
endif()

set(_he_sdk_gen "${CMAKE_BINARY_DIR}/HorizonSDK")
set(_he_sdk_targets "HorizonEngineTargets-${_he_sdk_profile}.cmake")
configure_file("${CMAKE_SOURCE_DIR}/cmake/sdk/HorizonEngineTargets.cmake.in"
               "${_he_sdk_gen}/${_he_sdk_targets}.genex" @ONLY)
file(GENERATE
    OUTPUT "${_he_sdk_gen}/$<CONFIG>/${_he_sdk_targets}"
    INPUT  "${_he_sdk_gen}/${_he_sdk_targets}.genex")
# The neutral config has no generator expression, so configure_file is enough.
configure_file("${CMAKE_SOURCE_DIR}/cmake/sdk/HorizonEngineConfig.cmake.in"
               "${_he_sdk_gen}/HorizonEngineConfig.cmake" @ONLY)
# ExactVersion: the libraries have no stable ABI between engine versions.
write_basic_package_version_file("${_he_sdk_gen}/HorizonEngineConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES
        "${_he_sdk_gen}/HorizonEngineConfig.cmake"
        "${_he_sdk_gen}/HorizonEngineConfigVersion.cmake"
        "${_he_sdk_gen}/$<CONFIG>/${_he_sdk_targets}"
        "${CMAKE_SOURCE_DIR}/cmake/sdk/HorizonEngineGame.cmake"
    DESTINATION ${_he_sdk_cmake_dir} COMPONENT ${_he_sdk_component})

# ── Header stamp ─────────────────────────────────────────────────────────────
# After every file above is in place (install scripts run in declaration
# order): one hash over include/ as it is now (but SDL_revision.h), written as the SDK's current
# stamp and as the stamp of this lib set. HorizonEngineConfig.cmake compares them.
install(CODE "set(_he_sdk_profile \"${_he_sdk_profile}\")" COMPONENT ${_he_sdk_component})
install(CODE [[
    set(_he_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
    file(GLOB_RECURSE _he_files LIST_DIRECTORIES false RELATIVE "${_he_root}/include" "${_he_root}/include/*")
    list(SORT _he_files)
    # SDL's generated revision string depends on the git state of each tree's
    # SDL fetch ("HEAD-HASH-NOTFOUND" in one, "release-3.2.14" in the other); no ABI.
    list(REMOVE_ITEM _he_files "SDL3/SDL_revision.h")
    set(_he_all "")
    foreach(_he_f IN LISTS _he_files)
        file(SHA256 "${_he_root}/include/${_he_f}" _he_h)
        string(APPEND _he_all "${_he_f} ${_he_h}\n")
    endforeach()
    string(SHA256 _he_stamp "${_he_all}")
    set(_he_pkg "${_he_root}/lib/cmake/HorizonEngine")
    file(WRITE "${_he_pkg}/HorizonEngineHeaders.txt" "${_he_stamp}\n")
    file(WRITE "${_he_pkg}/HorizonEngineHeaders-${_he_sdk_profile}.txt" "${_he_stamp}\n")
    message(STATUS "HorizonSDK: '${_he_sdk_profile}' lib set installed, header stamp ${_he_stamp}")
]] COMPONENT ${_he_sdk_component})

# ── The target ───────────────────────────────────────────────────────────────
# Not ALL: an engine build should not pay for a copy nobody asked for. The
# install script copies the files the dependencies below have just built.
add_custom_target(HorizonSDK
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --config $<CONFIG>
            --component ${_he_sdk_component} --prefix "${HE_ENGINE_SDK_DIR}"
    COMMENT "Installing the engine SDK's ${HE_BUILD_PROFILE} lib set into ${HE_ENGINE_SDK_DIR}"
    VERBATIM)
add_dependencies(HorizonSDK ${_he_sdk_libs} ${_he_sdl})
message(STATUS "HorizonEngine: SDK target HorizonSDK (${HE_BUILD_PROFILE} lib set) -> ${HE_ENGINE_SDK_DIR}")
