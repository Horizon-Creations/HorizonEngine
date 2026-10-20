# SDK probe project: built against an installed engine SDK only (see
# ../run_sdk_example.cmake). HE_SDK_PROFILE is the lib set to ask the package for
# (empty: let the package choose, i.e. the SDK's only one); the driver also asks
# for one the SDK lacks and expects find_package to refuse.
cmake_minimum_required(VERSION 3.20)
project(HorizonSdkProbe LANGUAGES CXX)

if(HE_SDK_PROFILE)
    find_package(HorizonEngine REQUIRED COMPONENTS ${HE_SDK_PROFILE})
else()
    find_package(HorizonEngine REQUIRED)
endif()
message(STATUS "sdk_probe: lib set '${HorizonEngine_BUILD_PROFILE}' of '${HorizonEngine_PROFILES}'")

add_executable(sdk_probe main.cpp)
target_link_libraries(sdk_probe PRIVATE
    HorizonEngine::Core HorizonEngine::Net HorizonEngine::Scene HorizonEngine::Rendering)
target_compile_definitions(sdk_probe PRIVATE HE_EXPECT_PROFILE="${HorizonEngine_BUILD_PROFILE}")
if(WIN32)
    foreach(_dep IN LISTS HorizonEngine_RUNTIME_TARGETS)
        add_custom_command(TARGET sdk_probe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${_dep}>" "$<TARGET_FILE_DIR:sdk_probe>"
            VERBATIM)
    endforeach()
endif()
