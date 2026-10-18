# Engine SDK end to end (Thema 211 step 5), run as `cmake -P`:
#   1. install the HorizonSDK component of the engine tree, then MOVE it (the
#      package must not refer back to where it was built or installed);
#   2. the probe project: find_package with the tree's profile, one call into each
#      library, run it; asking for the other profile must fail to configure;
#   3. the shipped template (share/HorizonEngine/template), i.e. HorizonGame built
#      from the SDK, with the golden HEGameBuildConfig.h compiled in.
#
# Variables (-D): HE_BUILD_DIR, HE_SOURCE_DIR, HE_WORK_DIR, HE_PROFILE,
# HE_CONFIG (build type), HE_GENERATOR [HE_GENERATOR_PLATFORM HE_MAKE_PROGRAM
# HE_CXX_COMPILER HE_OBJCXX_COMPILER] — forwarded so the nested projects use the
# same toolchain as the engine tree. HE_SKIP_GAME=1 leaves out step 3.
cmake_minimum_required(VERSION 3.20)

foreach(_v HE_BUILD_DIR HE_SOURCE_DIR HE_WORK_DIR HE_PROFILE HE_CONFIG HE_GENERATOR)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "run_sdk_example.cmake: -D${_v}=... is required")
    endif()
endforeach()

function(he_run what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${what} failed (${_rc}):\n${_out}")
    endif()
    message(STATUS "${what}: ok")
endfunction()

set(_gen_args -G "${HE_GENERATOR}")
if(HE_GENERATOR_PLATFORM)
    list(APPEND _gen_args -A "${HE_GENERATOR_PLATFORM}")
endif()
if(HE_MAKE_PROGRAM)
    list(APPEND _gen_args "-DCMAKE_MAKE_PROGRAM=${HE_MAKE_PROGRAM}")
endif()
if(HE_CXX_COMPILER)
    list(APPEND _gen_args "-DCMAKE_CXX_COMPILER=${HE_CXX_COMPILER}")
endif()
if(HE_OBJCXX_COMPILER)
    list(APPEND _gen_args "-DCMAKE_OBJCXX_COMPILER=${HE_OBJCXX_COMPILER}")
endif()
list(APPEND _gen_args "-DCMAKE_BUILD_TYPE=${HE_CONFIG}")

file(REMOVE_RECURSE "${HE_WORK_DIR}")
file(MAKE_DIRECTORY "${HE_WORK_DIR}")

# ── 1. install, move, look for paths that must not be in it ──────────────────
he_run("install HorizonSDK"
    "${CMAKE_COMMAND}" --install "${HE_BUILD_DIR}" --config "${HE_CONFIG}"
    --component HorizonSDK --prefix "${HE_WORK_DIR}/sdk-installed")
file(RENAME "${HE_WORK_DIR}/sdk-installed" "${HE_WORK_DIR}/sdk")
set(_sdk "${HE_WORK_DIR}/sdk")
set(_pkg "${_sdk}/lib/cmake/HorizonEngine")
foreach(_f HorizonEngineConfig.cmake HorizonEngineConfigVersion.cmake HorizonEngineGame.cmake)
    if(NOT EXISTS "${_pkg}/${_f}")
        message(FATAL_ERROR "SDK lacks lib/cmake/HorizonEngine/${_f}")
    endif()
    file(READ "${_pkg}/${_f}" _text)
    foreach(_p "${HE_BUILD_DIR}" "${HE_SOURCE_DIR}" "${HE_WORK_DIR}")
        string(FIND "${_text}" "${_p}" _at)
        if(NOT _at EQUAL -1)
            message(FATAL_ERROR "${_f} refers to ${_p}; the SDK must be relocatable")
        endif()
    endforeach()
endforeach()
foreach(_f include/Platform/BuildProfile.h include/Hpak/GameBuildConfig.h include/HorizonVersion.h
           include/glm/glm.hpp include/nlohmann/json.hpp include/SDL3/SDL.h include/SDL3/SDL_revision.h
           include/entt/entt.hpp share/HorizonEngine/game/GameApplication.cpp
           share/HorizonEngine/template/CMakeLists.txt)
    if(NOT EXISTS "${_sdk}/${_f}")
        message(FATAL_ERROR "SDK lacks ${_f}")
    endif()
endforeach()

# ── 2. probe ─────────────────────────────────────────────────────────────────
set(_probe_src "${HE_SOURCE_DIR}/tests/sdk_example/probe")
he_run("configure probe (${HE_PROFILE})"
    "${CMAKE_COMMAND}" -S "${_probe_src}" -B "${HE_WORK_DIR}/probe" ${_gen_args}
    "-DHorizonEngine_DIR=${_pkg}" "-DHE_SDK_PROFILE=${HE_PROFILE}")
he_run("build probe"
    "${CMAKE_COMMAND}" --build "${HE_WORK_DIR}/probe" --config "${HE_CONFIG}" --parallel 4)
file(GLOB_RECURSE _probe_exe "${HE_WORK_DIR}/probe/sdk_probe" "${HE_WORK_DIR}/probe/sdk_probe.exe")
if(NOT _probe_exe)
    message(FATAL_ERROR "the probe build produced no sdk_probe executable")
endif()
list(GET _probe_exe 0 _probe_exe)
execute_process(COMMAND "${_probe_exe}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
message(STATUS "sdk_probe: ${_out}")
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "profile=${HE_PROFILE} " OR NOT _out MATCHES "core=1 net=1 scene=1 rendering=1")
    message(FATAL_ERROR "sdk_probe failed (${_rc}):\n${_out}")
endif()

if(HE_PROFILE STREQUAL "dev")
    set(_other release)
else()
    set(_other dev)
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${_probe_src}" -B "${HE_WORK_DIR}/probe-${_other}" ${_gen_args}
            "-DHorizonEngine_DIR=${_pkg}" "-DHE_SDK_PROFILE=${_other}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
# CMake wraps the reason over several indented lines.
string(REGEX REPLACE "[ \t\r\n]+" " " _flat "${_out}")
if(_rc EQUAL 0 OR NOT _flat MATCHES "'${HE_PROFILE}' profile, '${_other}' was asked for")
    message(FATAL_ERROR "asking a ${HE_PROFILE} SDK for '${_other}' did not fail as it should (${_rc}):\n${_out}")
endif()
message(STATUS "probe asking for '${_other}': refused, as it should")

# ── 3. the template: HorizonGame from the SDK ────────────────────────────────
if(HE_SKIP_GAME)
    message(STATUS "game template skipped (HE_SKIP_GAME)")
    return()
endif()
he_run("configure template"
    "${CMAKE_COMMAND}" -S "${_sdk}/share/HorizonEngine/template" -B "${HE_WORK_DIR}/game" ${_gen_args}
    "-DHorizonEngine_DIR=${_pkg}" -DHE_GAME_NAME=SdkExampleGame
    "-DHE_GAME_BUILD_CONFIG=${HE_SOURCE_DIR}/tests/fixtures/game_build_config/HEGameBuildConfig.h")
he_run("build template"
    "${CMAKE_COMMAND}" --build "${HE_WORK_DIR}/game" --config "${HE_CONFIG}" --parallel 4)
file(GLOB_RECURSE _game_exe "${HE_WORK_DIR}/game/SdkExampleGame" "${HE_WORK_DIR}/game/SdkExampleGame.exe")
if(NOT _game_exe)
    message(FATAL_ERROR "the template build produced no SdkExampleGame executable")
endif()
message(STATUS "built ${_game_exe}")
