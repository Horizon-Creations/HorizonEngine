# SDK probe project: built against an installed engine SDK only (see
# ../run_sdk_example.cmake). HE_SDK_PROFILE is the profile to ask the package for;
# the driver also asks for the wrong one and expects find_package to refuse.
cmake_minimum_required(VERSION 3.20)
project(HorizonSdkProbe LANGUAGES CXX)

find_package(HorizonEngine REQUIRED COMPONENTS ${HE_SDK_PROFILE})

add_executable(sdk_probe main.cpp)
target_link_libraries(sdk_probe PRIVATE
    HorizonEngine::Core HorizonEngine::Net HorizonEngine::Scene HorizonEngine::Rendering)
target_compile_definitions(sdk_probe PRIVATE HE_EXPECT_PROFILE="${HE_SDK_PROFILE}")
if(WIN32)
    foreach(_dep IN LISTS HorizonEngine_RUNTIME_TARGETS)
        add_custom_command(TARGET sdk_probe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${_dep}>" "$<TARGET_FILE_DIR:sdk_probe>"
            VERBATIM)
    endforeach()
endif()
