# HorizonEngineGame.cmake — part of the engine SDK (lib/cmake/HorizonEngine), included
# by HorizonEngineConfig.cmake.
#
#   horizon_add_game(<target>
#       [OUTPUT_NAME <name>]                    executable name (default: <target>)
#       [BUILD_CONFIG <dir>/HEGameBuildConfig.h] compile the project configuration in
#       [SOURCES <file>...]                     more sources, e.g. generated HorizonCode
#       [PORTABLE])                             only the relative rpath, for shipping
#
# Builds the HorizonGame runtime from the sources in the SDK
# (share/HorizonEngine/game) against the lib set of the profile the project chose
# (HorizonEngine_BUILD_PROFILE, see HorizonEngineConfig.cmake). The target setup mirrors
# src/HE_Game/CMakeLists.txt, which builds the prebuilt runtime in the engine tree;
# a change to one belongs in the other. BUILD_CONFIG is the header
# HE::writeGameBuildConfigHeader generates (Hpak/GameBuildConfig.h): with it the
# game reads no project.hcfg / ProjectSettings.json, without it it does (the
# prebuilt runtime's behaviour).
#
# PORTABLE links the executable with its install rpath only
# (@executable_path/HorizonEngine, @executable_path; $ORIGIN on Linux): no absolute
# path to the SDK's lib/<profile> ends up in a binary that leaves this machine. It
# then only runs where its engine libraries sit beside it, as in an export folder,
# not from the build folder. The editor's export builds with it.
#
# On macOS the calling project must enable OBJCXX (the menu bar and notifications
# are Objective-C++): project(<name> LANGUAGES CXX OBJCXX).

include_guard(GLOBAL)

function(horizon_add_game target)
    cmake_parse_arguments(PARSE_ARGV 1 _hag "PORTABLE" "OUTPUT_NAME;BUILD_CONFIG" "SOURCES")
    if(_hag_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "horizon_add_game(${target}): unexpected arguments "
                            "'${_hag_UNPARSED_ARGUMENTS}'")
    endif()
    if(APPLE AND NOT CMAKE_OBJCXX_COMPILER_LOADED)
        message(FATAL_ERROR "horizon_add_game(${target}): enable Objective-C++ in the "
                            "project first: project(<name> LANGUAGES CXX OBJCXX)")
    endif()

    set(_src "${HorizonEngine_GAME_SOURCE_DIR}")
    set(_config_cpp "${_src}/CompiledBuildConfig.cpp")
    if(_hag_BUILD_CONFIG)
        get_filename_component(_bc "${_hag_BUILD_CONFIG}" ABSOLUTE)
        get_filename_component(_bc_name "${_bc}" NAME)
        if(NOT EXISTS "${_bc}" OR NOT _bc_name STREQUAL "HEGameBuildConfig.h")
            message(FATAL_ERROR "horizon_add_game(${target}): BUILD_CONFIG must name an "
                                "existing HEGameBuildConfig.h (got '${_hag_BUILD_CONFIG}')")
        endif()
        get_filename_component(_bc_dir "${_bc}" DIRECTORY)
        # A copy per target, so the definition and include dir set on it below do
        # not leak to another game of the same project through the shared source.
        set(_config_cpp "${CMAKE_CURRENT_BINARY_DIR}/${target}_build_config/CompiledBuildConfig.cpp")
        # The copy carries the header's hash, so a rewritten header changes the
        # SOURCE of this translation unit and every generator recompiles it.
        # OBJECT_DEPENDS alone is not enough: the Visual Studio generator ignores
        # it, and MSBuild's file tracker does not follow a per-source include dir,
        # so a kept build tree (the export's build cache) relinked the old
        # executable with the previous export's startup-scene UUID and pak key
        # compiled in — a game that mounts its pak and then finds no scene.
        file(SHA256 "${_bc}" _bc_hash)
        file(READ "${_src}/CompiledBuildConfig.cpp" _cbc_text)
        set(_cbc_text "${_cbc_text}\n// HEGameBuildConfig.h sha256: ${_bc_hash}\n")
        set(_cbc_old "")
        if(EXISTS "${_config_cpp}")
            file(READ "${_config_cpp}" _cbc_old)
        endif()
        if(NOT _cbc_old STREQUAL _cbc_text)
            file(WRITE "${_config_cpp}" "${_cbc_text}")
        endif()
        # OBJECT_DEPENDS: Make's dependency scan does not follow a per-source
        # include dir either (the hash above already forces the recompile).
        set_source_files_properties("${_config_cpp}" PROPERTIES
            INCLUDE_DIRECTORIES "${_bc_dir}"
            COMPILE_DEFINITIONS HE_GAME_BUILD_CONFIG=1
            OBJECT_DEPENDS "${_bc}")
    endif()

    add_executable(${target} WIN32
        "${_src}/main.cpp"
        "${_src}/GameApplication.cpp"
        "${_src}/EmbeddedPakKey.cpp"
        "${_config_cpp}"
        ${_hag_SOURCES})
    target_include_directories(${target} PRIVATE "${_src}")
    target_link_libraries(${target} PRIVATE HorizonEngine::HorizonEngine SDL3::SDL3)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    if(_hag_OUTPUT_NAME)
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${_hag_OUTPUT_NAME}")
    endif()

    if(APPLE)
        target_sources(${target} PRIVATE
            "${_src}/AppMacMenu.mm" "${_src}/AppNotify.mm")
        set_source_files_properties("${_src}/AppMacMenu.mm" "${_src}/AppNotify.mm"
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(${target} PRIVATE "-framework Cocoa" "-framework UserNotifications")
        # HorizonEngine/ beside the executable (the export layout), then the
        # executable's own folder; the build rpath adds the SDK's lib/<profile>.
        set_target_properties(${target} PROPERTIES
            OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON
            BUILD_RPATH "@executable_path/HorizonEngine;@executable_path"
            INSTALL_RPATH "@executable_path/HorizonEngine;@executable_path")
    elseif(UNIX)
        set_target_properties(${target} PROPERTIES
            BUILD_RPATH "$ORIGIN/HorizonEngine;$ORIGIN" INSTALL_RPATH "$ORIGIN/HorizonEngine;$ORIGIN")
    endif()
    if(_hag_PORTABLE)
        set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH ON)
    endif()
    # Windows: the engine and SDL3 delay-loaded from HorizonEngine\ beside the
    # executable (EngineDllDir.cpp), as in the engine tree's HE_Game.
    if(MSVC)
        target_sources(${target} PRIVATE "${_src}/EngineDllDir.cpp")
        target_link_options(${target} PRIVATE
            "/DELAYLOAD:$<TARGET_FILE_NAME:HorizonEngine::HorizonEngine>"
            "/DELAYLOAD:$<TARGET_FILE_NAME:SDL3::SDL3>")
        target_link_libraries(${target} PRIVATE delayimp)
    endif()

    if(HorizonEngine_VULKAN_ENABLED)
        target_compile_definitions(${target} PRIVATE HE_VULKAN_ENABLED)
    endif()
    # The dev profile's runtime choice, as in the engine tree; a release lib set
    # has no hotkey code at all (HE_FEATURE_DEV_HOTKEYS=0), whatever is defined.
    if(HorizonEngine_BUILD_PROFILE STREQUAL "dev")
        target_compile_definitions(${target} PRIVATE
            $<$<CONFIG:Debug,RelWithDebInfo>:HE_GAME_DEV_HOTKEYS>)
    endif()

    # Windows has no rpath: the DLLs go beside the executable for a run from the
    # build folder (the delay-load hook falls back to the normal search when there
    # is no HorizonEngine\). Elsewhere the build rpath reaches the SDK's
    # lib/<profile> (and the export swaps the libraries into its HorizonEngine/
    # folder, where the rpath above finds them). Runtime data (Shaders/) is not in the SDK: it comes with the
    # prebuilt runtime's export folder the built game is swapped into.
    if(WIN32)
        foreach(_dep IN LISTS HorizonEngine_RUNTIME_TARGETS)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:${_dep}>" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endforeach()
    endif()
endfunction()
