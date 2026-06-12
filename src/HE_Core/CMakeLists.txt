add_library(HorizonCore SHARED
	src/Application/Application.cpp
	src/Application/GameLoop.cpp
	src/Application/GameLogicLoader.cpp
	src/Application/Input.cpp
	src/Diagnostics/GlobalState.cpp
	src/Diagnostics/Logger.cpp
	src/Diagnostics/Profiler.cpp
	src/Diagnostics/CrashHandler.cpp
	src/Memory/PoolAllocator.cpp
	src/Memory/Ref.cpp
	src/Types/UUID.cpp
	src/Types/StringID.cpp
	src/Platform/FileSystem.cpp
	src/Platform/DynLib.cpp
	src/Window/Window.cpp
	src/Renderer/IRenderer.cpp
 "include/ContentManager/ContentManager.h" "src/ContentManager/ContentManager.cpp" "include/ContentManager/Assets.h")

target_include_directories(HorizonCore
	PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(HorizonCore PUBLIC cxx_std_20)

target_compile_definitions(HorizonCore
	PRIVATE HE_CORE_BUILD_DLL
	PUBLIC  HE_CORE_DLL
)

target_link_libraries(HorizonCore
	PUBLIC  glm::glm
	PUBLIC  SDL3::SDL3
	PUBLIC  nlohmann_json::nlohmann_json
)

he_deploy_dll(HorizonCore)
