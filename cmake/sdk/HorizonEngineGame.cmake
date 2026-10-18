# HorizonEngineGame.cmake — part of the engine SDK (lib/cmake/HorizonEngine), included
# by HorizonEngineConfig.cmake.
#
#   horizon_add_game(<target>
#       [OUTPUT_NAME <name>]                    executable name (default: <target>)
#       [BUILD_CONFIG <dir>/HEGameBuildConfig.h] compile the project configuration in
#       [SOURCES <file>...])                    more sources, e.g. generated HorizonCode
#
# Builds the HorizonGame runtime from the sources in the SDK
# (share/HorizonEngine/game) against the SDK's libraries. The target setup mirrors
# src/HE_Game/CMakeLists.txt, which builds the prebuilt runtime in the engine tree;
# a change to one belongs in the other. BUILD_CONFIG is the header
# HE::writeGameBuildConfigHeader generates (Hpak/GameBuildConfig.h): with it the
# game reads no project.hcfg / ProjectSettings.json, without it it does (the
# prebuilt runtime's behaviour).
#
# On macOS the calling project must enable OBJCXX (the menu bar and notifications
# are Objective-C++): project(<name> LANGUAGES CXX OBJCXX).

include_guard(GLOBAL)

function(horizon_add_game target)
    cmake_parse_arguments(PARSE_ARGV 1 _hag "" "OUTPUT_NAME;BUILD_CONFIG" "SOURCES")
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
        configure_file("${_src}/CompiledBuildConfig.cpp" "${_config_cpp}" COPYONLY)
        set_source_files_properties("${_config_cpp}" PROPERTIES
            INCLUDE_DIRECTORIES "${_bc_dir}"
            COMPILE_DEFINITIONS HE_GAME_BUILD_CONFIG=1)
    endif()

    add_executable(${target} WIN32
        "${_src}/main.cpp"
        "${_src}/GameApplication.cpp"
        "${_src}/EmbeddedPakKey.cpp"
        "${_config_cpp}"
        ${_hag_SOURCES})
    target_include_directories(${target} PRIVATE "${_src}")
    target_link_libraries(${target} PRIVATE
        HorizonEngine::Core HorizonEngine::Scene HorizonEngine::Rendering SDL3::SDL3)
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
        set_target_properties(${target} PROPERTIES
            OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON
            BUILD_RPATH "@executable_path" INSTALL_RPATH "@executable_path")
    elseif(UNIX)
        set_target_properties(${target} PROPERTIES
            BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    endif()

    if(HorizonEngine_VULKAN_ENABLED)
        target_compile_definitions(${target} PRIVATE HE_VULKAN_ENABLED)
    endif()
    # The dev profile's runtime choice, as in the engine tree; a release package
    # has no hotkey code at all (HE_FEATURE_DEV_HOTKEYS=0), whatever is defined.
    if(HorizonEngine_BUILD_PROFILE STREQUAL "dev")
        target_compile_definitions(${target} PRIVATE
            $<$<CONFIG:Debug,RelWithDebInfo>:HE_GAME_DEV_HOTKEYS>)
    endif()

    # Windows has no rpath: the DLLs go beside the executable. Elsewhere the
    # build rpath reaches the SDK's lib/ (and the exporter copies the libraries
    # beside the shipped executable, where @executable_path / $ORIGIN finds them).
    if(WIN32)
        foreach(_dep IN LISTS HorizonEngine_RUNTIME_TARGETS)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:${_dep}>" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endforeach()
    endif()
    if(HorizonEngine_HAS_SHADERS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${HorizonEngine_SHADER_DIR}" "$<TARGET_FILE_DIR:${target}>/Shaders"
            VERBATIM)
    endif()
endfunction()
