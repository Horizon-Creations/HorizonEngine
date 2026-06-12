cmake_minimum_required(VERSION 3.20)
project(HorizonEngine VERSION 1.0 LANGUAGES CXX)

# Objective-C++ is needed for the Metal backend and the ImGui Metal bridge.
# Must be enabled at the top level — enable_language() in subdirectories does
# not reliably propagate with all generators.
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
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

add_subdirectory(src/HE_Core)
add_subdirectory(src/HE_Scene)
add_subdirectory(src/HE_Rendering)
add_subdirectory(src/HE_Tools)
add_subdirectory(src/HE_Game)
add_subdirectory(src/HE_Editor)
