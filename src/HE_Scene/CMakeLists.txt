add_library(HorizonScene SHARED
    src/HorizonWorld.cpp
    src/GameInstanceHost.cpp
    src/PlayerHost.cpp
    src/SceneGraph.cpp
    src/SceneSerializer.cpp
    src/Components/TransformComponent.cpp
    src/TerrainMeshGenerator.cpp
    src/TerrainSystem.cpp
    src/AnimationEval.cpp
    src/AnimationSystem.cpp
    src/AnimationBlendSystem.cpp
    src/AnimationStateMachineSystem.cpp
    src/AnimationPreview.cpp
    src/PropertyAnimationSystem.cpp
    src/NavigationSystem.cpp
    src/ParticleSystem.cpp
    src/LODSystem.cpp
    src/WeatherSystem.cpp
    src/SceneSystems.cpp
    src/FoliageSystem.cpp
    src/UISystem.cpp
    src/UIInputSystem.cpp
    src/WidgetManager.cpp
    src/ScriptContext.cpp
    src/ScriptApi.cpp
    src/EngineApi.cpp
    src/HcCodegen.cpp
    src/PyScriptBackend.cpp
    src/AudioEngine.cpp
    src/PhysicsWorld.cpp
)

target_include_directories(HorizonScene
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${lua_SOURCE_DIR}/lua-5.4.7/include
)

target_compile_definitions(HorizonScene
    PRIVATE HE_SCENE_BUILD_DLL
    PUBLIC  HE_SCENE_DLL
)

target_compile_features(HorizonScene PUBLIC cxx_std_20)

target_link_libraries(HorizonScene
    PUBLIC  HorizonCore
    PRIVATE nlohmann_json::nlohmann_json
            Jolt
            RecastNavigation::Recast
            RecastNavigation::Detour
)

if(APPLE)
    target_link_libraries(HorizonScene PRIVATE
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework AudioToolbox"
    )
endif()

if(HE_HAVE_PYTHON)
    target_link_libraries(HorizonScene PRIVATE Python3::Python)
    target_compile_definitions(HorizonScene PRIVATE HE_HAVE_PYTHON)
endif()

set_target_properties(HorizonScene PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

he_deploy_dll(HorizonScene)
