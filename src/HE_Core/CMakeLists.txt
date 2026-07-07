add_library(HorizonCore SHARED
	src/Application/Application.cpp
	src/Application/GameLoop.cpp
	src/Application/GameLogicLoader.cpp
	src/Application/Input.cpp
	src/Application/InputMapping.cpp
	src/Diagnostics/GlobalState.cpp
	src/Diagnostics/Logger.cpp
	src/Diagnostics/Profiler.cpp
	src/Diagnostics/EngineProfiler.cpp
	src/Diagnostics/CrashHandler.cpp
	src/Memory/PoolAllocator.cpp
	src/Memory/Ref.cpp
	src/JobSystem/JobSystem.cpp
	src/Types/UUID.cpp
	src/Types/StringID.cpp
	src/Platform/FileSystem.cpp
	src/Platform/DynLib.cpp
	src/Window/Window.cpp
	src/Renderer/IRenderer.cpp
	src/Scripting/ScriptEngine.cpp
	src/Hpak/HpakWriter.cpp
	src/Hpak/HpakReader.cpp
	src/Hpak/Aes256Gcm.cpp
	src/Hpak/KeyDerivation.cpp
	src/Hpak/ProjectConfig.cpp
	src/Hpak/ProjectExporter.cpp
	src/Events/EventBus.cpp
	src/MaterialGraph/MaterialGraph.cpp
	src/UIWidget/UIWidgetTree.cpp
	src/UIWidget/UIElement.cpp
	src/UIWidget/UIWidgetBinding.cpp
	src/HorizonCode/HorizonCode.cpp
	src/HorizonCode/HorizonCodeRuntime.cpp
	src/Renderer/UIFont.cpp
 "include/ContentManager/ContentManager.h" "src/ContentManager/ContentManager.cpp" "include/ContentManager/Assets.h")

target_include_directories(HorizonCore
	PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
	        ${CMAKE_CURRENT_SOURCE_DIR}/vendor
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
	PUBLIC  lua_static
)
if(HE_HAVE_LZ4)
    target_link_libraries(HorizonCore PRIVATE lz4::lz4)
    target_compile_definitions(HorizonCore PRIVATE HE_HAVE_LZ4)
endif()
if(HE_HAVE_ZSTD)
    target_link_libraries(HorizonCore PRIVATE zstd::zstd)
    target_compile_definitions(HorizonCore PRIVATE HE_HAVE_ZSTD)
endif()
if(HE_HAVE_OPENSSL)
    target_link_libraries(HorizonCore PRIVATE OpenSSL::Crypto)
    target_compile_definitions(HorizonCore PRIVATE HE_HAVE_OPENSSL)
endif()
if(HE_HAVE_ASTCENC)
    target_link_libraries(HorizonCore PRIVATE astcenc-native-static)
    target_include_directories(HorizonCore PRIVATE ${astcenc_SOURCE_DIR}/Source)
    target_compile_definitions(HorizonCore PRIVATE HE_HAVE_ASTCENC)
endif()

# Tracy::TracyClient propagates TRACY_ENABLE define + include dir to all
# downstream consumers (HorizonRendering, HorizonEditor, etc.) via PUBLIC linkage.
if(HE_ENABLE_TRACY)
    target_link_libraries(HorizonCore PUBLIC Tracy::TracyClient)
endif()

he_deploy_dll(HorizonCore)
