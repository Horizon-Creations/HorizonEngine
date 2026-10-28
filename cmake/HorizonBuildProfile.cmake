# ─── Build profile: dev | release (Thema 211, step 2) ─────────────────────────
#
# The profile decides at COMPILE time whether developer features exist in a
# binary at all: the F9 profiler capture, the headless/test environment hooks,
# the game's V/F10 hotkeys, the HE_* environment overrides. A runtime switch
# could only hide them; a release build must not contain the code.
#
#   dev      everything a developer, a test or a perf script needs. The editor,
#            he_tests and the prebuilt runtimes the exporter copies are dev builds.
#   release  what an exported game built from the SDK ships: no dev features.
#
# Like HE_RUNTIME_FLAVOR this is a property of the whole TREE, not of one
# target. Dev code also sits in the engine libraries (HorizonCore's F9 key,
# Window's HE_HIDDEN_WINDOW), so a release game needs release-built libraries:
# the SDK ships one variant per profile, each from its own build directory.
# Mixing a release game with dev libraries would leave the dev code in the
# libraries, and profile-dependent inline code in a header would break the
# one-definition rule. Hence: one profile per link unit.
#
# The macros, their names and how to use them: src/HE_Core/include/Platform/BuildProfile.h
# and docs/coding-conventions.md section 9. The per-profile DEFAULT of every
# feature lives in that header only; this file can only override one.
#
# This module reads no variable of the engine's root CMakeLists, so the SDK
# (step 5) can ship it unchanged next to HorizonEngineConfig.cmake.

include_guard(GLOBAL)

set(HE_BUILD_PROFILE "dev" CACHE STRING "Build profile of this tree: dev | release")
set_property(CACHE HE_BUILD_PROFILE PROPERTY STRINGS dev release)
if(NOT HE_BUILD_PROFILE MATCHES "^(dev|release)$")
    message(FATAL_ERROR "HE_BUILD_PROFILE must be dev or release (got '${HE_BUILD_PROFILE}')")
endif()

# Every feature switch BuildProfile.h knows, without the HE_FEATURE_ prefix.
# Only used to reject a misspelt override; add a name here and in the header.
set(HE_BUILD_FEATURES
    DEV_HOTKEYS
    PROFILER_CAPTURE
    TEST_HOOKS
    ENV_OVERRIDES)

# he_target_build_profile(<target> [PUBLIC|PRIVATE|INTERFACE]
#                         [PROFILE dev|release]
#                         [FEATURES <NAME>=0|1 ...])
#
# Gives <target> HE_BUILD_PROFILE_DEV / HE_BUILD_PROFILE_RELEASE (always both,
# as 0 or 1) and one HE_FEATURE_<NAME> per override. Scope defaults to PRIVATE;
# an engine library uses PUBLIC so whatever links it compiles its headers with
# the same profile.
#
# PROFILE defaults to the tree's HE_BUILD_PROFILE. Passing another one is for
# isolated probes only (tests/build_profile_probe_*.cpp): the result must not
# share a header with profile-dependent inline code with anything it links to.
function(he_target_build_profile target)
    cmake_parse_arguments(PARSE_ARGV 1 _hbp "" "PROFILE" "FEATURES")
    # The optional scope word comes before any keyword, so it lands in the
    # unparsed arguments.
    set(_scope PRIVATE)
    if(_hbp_UNPARSED_ARGUMENTS)
        list(GET _hbp_UNPARSED_ARGUMENTS 0 _first)
        if(_first MATCHES "^(PUBLIC|PRIVATE|INTERFACE)$")
            set(_scope ${_first})
            list(REMOVE_AT _hbp_UNPARSED_ARGUMENTS 0)
        endif()
    endif()
    if(_hbp_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "he_target_build_profile(${target}): unexpected "
                            "arguments '${_hbp_UNPARSED_ARGUMENTS}'")
    endif()

    set(_profile "${HE_BUILD_PROFILE}")
    if(DEFINED _hbp_PROFILE)
        string(TOLOWER "${_hbp_PROFILE}" _profile)
    endif()
    if(_profile STREQUAL "dev")
        set(_defs HE_BUILD_PROFILE_DEV=1 HE_BUILD_PROFILE_RELEASE=0)
    elseif(_profile STREQUAL "release")
        set(_defs HE_BUILD_PROFILE_DEV=0 HE_BUILD_PROFILE_RELEASE=1)
    else()
        message(FATAL_ERROR "he_target_build_profile(${target}): PROFILE must be "
                            "dev or release (got '${_hbp_PROFILE}')")
    endif()

    foreach(_f IN LISTS _hbp_FEATURES)
        if(NOT _f MATCHES "^([A-Z0-9_]+)=([01])$")
            message(FATAL_ERROR "he_target_build_profile(${target}): feature "
                                "'${_f}' must read NAME=0 or NAME=1")
        endif()
        set(_name "${CMAKE_MATCH_1}")
        set(_value "${CMAKE_MATCH_2}")
        if(NOT _name IN_LIST HE_BUILD_FEATURES)
            message(FATAL_ERROR "he_target_build_profile(${target}): unknown feature "
                                "'${_name}' (known: ${HE_BUILD_FEATURES})")
        endif()
        list(APPEND _defs HE_FEATURE_${_name}=${_value})
    endforeach()

    target_compile_definitions(${target} ${_scope} ${_defs})
    set_property(TARGET ${target} PROPERTY HE_BUILD_PROFILE "${_profile}")
endfunction()

message(STATUS "HorizonEngine: build profile '${HE_BUILD_PROFILE}'")
