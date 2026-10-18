# A minimal game project against the Horizon Engine SDK
# (share/HorizonEngine/template in the SDK; cmake/sdk/template in the engine tree).
#
#   cmake -S <this dir> -B build -DHorizonEngine_DIR=<sdk>/lib/cmake/HorizonEngine
#         [-DHE_GAME_BUILD_CONFIG=<dir>/HEGameBuildConfig.h] [-DHE_GAME_NAME=MyGame]
#   cmake --build build
#
# Without HE_GAME_BUILD_CONFIG the game reads project.hcfg beside itself, like the
# prebuilt runtime; with it the project configuration and the pak key are
# compiled in. The profile (dev | release) is the SDK's: pick the SDK, not a flag.
cmake_minimum_required(VERSION 3.20)

set(HE_GAME_NAME "HorizonGame" CACHE STRING "Name of the game executable")
set(HE_GAME_BUILD_CONFIG "" CACHE FILEPATH
    "HEGameBuildConfig.h to compile in (empty = read project.hcfg at runtime)")

if(APPLE)
    project(HorizonGameProject LANGUAGES CXX OBJCXX)
else()
    project(HorizonGameProject LANGUAGES CXX)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(HorizonEngine REQUIRED)
message(STATUS "Horizon Engine ${HorizonEngine_VERSION} SDK, profile '${HorizonEngine_BUILD_PROFILE}'")

if(HE_GAME_BUILD_CONFIG)
    horizon_add_game(${HE_GAME_NAME} BUILD_CONFIG "${HE_GAME_BUILD_CONFIG}")
else()
    horizon_add_game(${HE_GAME_NAME})
endif()
