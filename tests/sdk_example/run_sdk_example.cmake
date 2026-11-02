# Engine SDK end to end (Thema 211 steps 5 and 9), run as `cmake -P`:
#   1. install the HorizonSDK component of the engine tree (and of a second tree
#      of the other profile, when given) into ONE prefix, then MOVE it (the
#      package must not refer back to where it was built or installed);
#   2. the SDK holds headers, lib sets, the game sources and the package, and
#      none of the pruned third-party headers, which no shipped header includes;
#   3. the probe project, per installed lib set: find_package with that profile,
#      one call into each library, run it; then the choices the package must
#      refuse: a lib set it lacks, no choice when it holds two, a lib set whose
#      header stamp differs from include/;
#   4. the shipped template (share/HorizonEngine/template), i.e. HorizonGame built
#      from the SDK as the export does it (HE_BUILD_PROFILE + the golden
#      HEGameBuildConfig.h), for every installed lib set.
#
# Variables (-D): HE_BUILD_DIR, HE_SOURCE_DIR, HE_WORK_DIR, HE_PROFILE,
# HE_CONFIG (build type), HE_GENERATOR [HE_GENERATOR_PLATFORM HE_MAKE_PROGRAM
# HE_CXX_COMPILER HE_OBJCXX_COMPILER] — forwarded so the nested projects use the
# same toolchain as the engine tree. HE_BUILD_DIR_2 + HE_PROFILE_2: a second tree
# (the other profile, same sources) whose lib set joins the same SDK; the ctest
# has one tree, a two-set run is by hand. HE_SKIP_GAME=1 leaves out step 4.
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

set(_trees "${HE_BUILD_DIR}")
set(_profiles "${HE_PROFILE}")
if(HE_BUILD_DIR_2)
    if(NOT HE_PROFILE_2 OR HE_PROFILE_2 STREQUAL HE_PROFILE)
        message(FATAL_ERROR "HE_BUILD_DIR_2 needs HE_PROFILE_2, the other profile")
    endif()
    list(APPEND _trees "${HE_BUILD_DIR_2}")
    list(APPEND _profiles "${HE_PROFILE_2}")
endif()

file(REMOVE_RECURSE "${HE_WORK_DIR}")
file(MAKE_DIRECTORY "${HE_WORK_DIR}")

# ── 1. install every lib set into one prefix, move it ────────────────────────
foreach(_tree _p IN ZIP_LISTS _trees _profiles)
    he_run("install HorizonSDK (${_p} lib set)"
        "${CMAKE_COMMAND}" --install "${_tree}" --config "${HE_CONFIG}"
        --component HorizonSDK --prefix "${HE_WORK_DIR}/sdk-installed")
endforeach()
file(RENAME "${HE_WORK_DIR}/sdk-installed" "${HE_WORK_DIR}/sdk")
set(_sdk "${HE_WORK_DIR}/sdk")
set(_pkg "${_sdk}/lib/cmake/HorizonEngine")
set(_pkg_files HorizonEngineConfig.cmake HorizonEngineConfigVersion.cmake HorizonEngineGame.cmake
               HorizonEngineHeaders.txt)
foreach(_p IN LISTS _profiles)
    list(APPEND _pkg_files HorizonEngineTargets-${_p}.cmake HorizonEngineHeaders-${_p}.txt)
endforeach()
foreach(_f IN LISTS _pkg_files)
    if(NOT EXISTS "${_pkg}/${_f}")
        message(FATAL_ERROR "SDK lacks lib/cmake/HorizonEngine/${_f}")
    endif()
    file(READ "${_pkg}/${_f}" _text)
    foreach(_p IN LISTS _trees ITEMS "${HE_SOURCE_DIR}" "${HE_WORK_DIR}")
        string(FIND "${_text}" "${_p}" _at)
        if(NOT _at EQUAL -1)
            message(FATAL_ERROR "${_f} refers to ${_p}; the SDK must be relocatable")
        endif()
    endforeach()
endforeach()

# ── 2. what is in it, and what is not ────────────────────────────────────────
set(_need include/Platform/BuildProfile.h include/Hpak/GameBuildConfig.h include/HorizonVersion.h
          include/HorizonEngine/HorizonEngineApi.h
          include/glm/glm.hpp include/nlohmann/json.hpp include/SDL3/SDL.h include/SDL3/SDL_revision.h
          include/entt/entt.hpp share/HorizonEngine/game/GameApplication.cpp
          share/HorizonEngine/template/CMakeLists.txt)
foreach(_p IN LISTS _profiles)
    list(APPEND _need lib/${_p})
    # The engine as one library (step 10), none of the four it is made of.
    file(GLOB _engine "${_sdk}/lib/${_p}/*HorizonEngine*" "${_sdk}/bin/${_p}/*HorizonEngine*")
    file(GLOB _parts "${_sdk}/lib/${_p}/*HorizonCore*" "${_sdk}/bin/${_p}/*HorizonCore*"
                     "${_sdk}/lib/${_p}/*HorizonScene*" "${_sdk}/bin/${_p}/*HorizonScene*")
    if(NOT _engine)
        message(FATAL_ERROR "SDK lib set '${_p}' has no HorizonEngine library")
    endif()
    if(_parts)
        message(FATAL_ERROR "SDK lib set '${_p}' ships the separate engine libraries: ${_parts}")
    endif()
endforeach()
foreach(_f IN LISTS _need)
    if(NOT EXISTS "${_sdk}/${_f}")
        message(FATAL_ERROR "SDK lacks ${_f}")
    endif()
endforeach()
# Pruned (cmake/HorizonSDK.cmake, "Headers"): third-party headers nothing the
# compiler reaches includes, runtime data, a flat lib/ from the per-profile SDK.
file(GLOB_RECURSE _core_vendor "${HE_SOURCE_DIR}/src/HE_Core/vendor/*.h")
set(_pruned miniaudio.h SDL_opengl.h SDL_opengl_glext.h SDL_opengles2.h SDL_egl.h SDL_test.h SDL_test_harness.h)
foreach(_f IN LISTS _core_vendor)
    get_filename_component(_f "${_f}" NAME)
    list(APPEND _pruned "${_f}")
endforeach()
foreach(_f IN LISTS _pruned)
    file(GLOB_RECURSE _hit "${_sdk}/include/${_f}")
    if(_hit)
        message(FATAL_ERROR "SDK ships ${_hit}, which the compiler does not need")
    endif()
endforeach()
# By path, not by name (glm/fwd.hpp and entt/entt.hpp are shipped): the loose
# copies beside HE_Scene/vendor/entt, runtime data, the flat lib/ of the old
# one-SDK-per-profile layout.
foreach(_f include/entt.hpp include/fwd.hpp include/bc7enc.cpp share/HorizonEngine/Shaders
           lib/libHorizonCore.dylib lib/libHorizonCore.so bin/HorizonCore.dll)
    if(EXISTS "${_sdk}/${_f}")
        message(FATAL_ERROR "SDK ships ${_f}, which belongs to no lib set / is no compiler input")
    endif()
endforeach()
# ... and no shipped header includes one of them (a public header that starts
# to would compile in the tree and break every SDK build).
string(REPLACE "." "\\." _pruned_rx "${_pruned}")
list(JOIN _pruned_rx "|" _pruned_rx)
file(GLOB_RECURSE _headers "${_sdk}/include/*")
set(_bad "")
foreach(_h IN LISTS _headers)
    file(STRINGS "${_h}" _inc REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]*/)?(${_pruned_rx})[>\"]")
    if(_inc)
        list(APPEND _bad "${_h}: ${_inc}")
    endif()
endforeach()
if(_bad)
    list(JOIN _bad "\n  " _bad)
    message(FATAL_ERROR "shipped headers include pruned ones; ship them in HorizonSDK.cmake:\n  ${_bad}")
endif()
message(STATUS "SDK contents: ok (${_profiles} lib sets, pruned headers absent and unreferenced)")

# ── 3. probe ─────────────────────────────────────────────────────────────────
set(_probe_src "${HE_SOURCE_DIR}/tests/sdk_example/probe")

# Configures the probe asking for `ask` (empty: no choice) and expects a refusal
# whose (unwrapped) text matches `rx`.
function(he_expect_refusal tag ask rx)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_probe_src}" -B "${HE_WORK_DIR}/probe-${tag}" ${_gen_args}
                "-DHorizonEngine_DIR=${_pkg}" "-DHE_SDK_PROFILE=${ask}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    # CMake wraps the reason over several indented lines.
    string(REGEX REPLACE "[ \t\r\n]+" " " _flat "${_out}")
    if(_rc EQUAL 0 OR NOT _flat MATCHES "${rx}")
        message(FATAL_ERROR "probe (${tag}) was not refused as it should (${_rc}):\n${_out}")
    endif()
    message(STATUS "probe (${tag}): refused, as it should")
endfunction()

foreach(_p IN LISTS _profiles)
    he_run("configure probe (${_p})"
        "${CMAKE_COMMAND}" -S "${_probe_src}" -B "${HE_WORK_DIR}/probe-${_p}" ${_gen_args}
        "-DHorizonEngine_DIR=${_pkg}" "-DHE_SDK_PROFILE=${_p}")
    he_run("build probe (${_p})"
        "${CMAKE_COMMAND}" --build "${HE_WORK_DIR}/probe-${_p}" --config "${HE_CONFIG}" --parallel 4)
    file(GLOB_RECURSE _probe_exe "${HE_WORK_DIR}/probe-${_p}/sdk_probe" "${HE_WORK_DIR}/probe-${_p}/sdk_probe.exe")
    if(NOT _probe_exe)
        message(FATAL_ERROR "the probe build (${_p}) produced no sdk_probe executable")
    endif()
    list(GET _probe_exe 0 _probe_exe)
    execute_process(COMMAND "${_probe_exe}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    message(STATUS "sdk_probe (${_p}): ${_out}")
    if(NOT _rc EQUAL 0 OR NOT _out MATCHES "profile=${_p} " OR NOT _out MATCHES "core=1 net=1 scene=1 rendering=1 api=1")
        message(FATAL_ERROR "sdk_probe (${_p}) failed (${_rc}):\n${_out}")
    endif()
endforeach()

list(LENGTH _profiles _n)
if(_n EQUAL 1)
    # No choice: the only lib set.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_probe_src}" -B "${HE_WORK_DIR}/probe-default" ${_gen_args}
                "-DHorizonEngine_DIR=${_pkg}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    if(NOT _rc EQUAL 0 OR NOT _out MATCHES "sdk_probe: lib set '${HE_PROFILE}' of '${HE_PROFILE}'")
        message(FATAL_ERROR "the probe with no profile did not get the only lib set (${_rc}):\n${_out}")
    endif()
    message(STATUS "probe with no profile: got '${HE_PROFILE}', the only lib set")
    if(HE_PROFILE STREQUAL "dev")
        set(_other release)
    else()
        set(_other dev)
    endif()
    he_expect_refusal(${_other} ${_other} "has no '${_other}' lib set \\(installed: ${HE_PROFILE}\\)\\. Build")
else()
    he_expect_refusal(none "" "holds the lib sets dev, release\\. Choose one")
endif()
# A lib set installed with other headers (as if another build had installed
# over include/ since): its stamp no longer matches.
list(GET _profiles 0 _p0)
set(_stamp "${_pkg}/HorizonEngineHeaders-${_p0}.txt")
file(READ "${_stamp}" _saved)
file(WRITE "${_stamp}" "0000\n")
he_expect_refusal(stale-${_p0} ${_p0} "'${_p0}' lib set .* was installed with other headers")
file(WRITE "${_stamp}" "${_saved}")

# ── 4. the template: HorizonGame from the SDK, as the export builds it ───────
if(HE_SKIP_GAME)
    message(STATUS "game template skipped (HE_SKIP_GAME)")
    return()
endif()
foreach(_p IN LISTS _profiles)
    he_run("configure template (${_p})"
        "${CMAKE_COMMAND}" -S "${_sdk}/share/HorizonEngine/template" -B "${HE_WORK_DIR}/game-${_p}" ${_gen_args}
        "-DHorizonEngine_DIR=${_pkg}" "-DHE_BUILD_PROFILE=${_p}" -DHE_GAME_NAME=SdkExampleGame
        "-DHE_GAME_BUILD_CONFIG=${HE_SOURCE_DIR}/tests/fixtures/game_build_config/HEGameBuildConfig.h")
    he_run("build template (${_p})"
        "${CMAKE_COMMAND}" --build "${HE_WORK_DIR}/game-${_p}" --config "${HE_CONFIG}" --parallel 4)
    file(GLOB_RECURSE _game_exe "${HE_WORK_DIR}/game-${_p}/SdkExampleGame" "${HE_WORK_DIR}/game-${_p}/SdkExampleGame.exe")
    if(NOT _game_exe)
        message(FATAL_ERROR "the template build (${_p}) produced no SdkExampleGame executable")
    endif()
    message(STATUS "built ${_game_exe}")
endforeach()
