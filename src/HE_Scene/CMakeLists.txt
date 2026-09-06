add_library(HorizonScene SHARED
	src/GameReplication.cpp
    src/HorizonWorld.cpp
    src/GameInstanceHost.cpp
    src/PlayerHost.cpp
    src/EntityHost.cpp
    src/AnimatorHost.cpp
    src/MovementSystem.cpp
    src/SceneSerializer.cpp
    src/Components/TransformComponent.cpp
    src/TerrainMeshGenerator.cpp
    src/TerrainSystem.cpp
    src/TerrainPaint.cpp
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
    src/EnvironmentPush.cpp
    src/FlyCameraController.cpp
    src/TransformHierarchy.cpp
    src/CameraRigController.cpp
    src/CameraShake.cpp
    src/FoliageSystem.cpp
    src/UISystem.cpp
    src/UIInputSystem.cpp
    src/WidgetManager.cpp
    src/ScriptContext.cpp
    src/ScriptApi.cpp
    src/EngineApi.cpp
    src/HcCodegen.cpp
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
    # Gameplay replication (GameReplication) sits on the shared transport.
    # HorizonNet is BELOW the scene layer, so this dependency points downward.
    PUBLIC  HorizonNet
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

# NOTE: HorizonScene deliberately does NOT link Python3::Python any more. The
# CPython backend lives in the HorizonPython plugin and is dlopen'd at runtime
# (see src/HE_Python and Scripting/PythonPluginAbi.h), which is what keeps
# libpython out of this library's load-time dependencies — and therefore out of
# every shipped game that does not use Python. Do not add the link back; adding
# it would silently restore the ~5-9 MB per platform that the split removed.

set_target_properties(HorizonScene PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

he_deploy_dll(HorizonScene)
