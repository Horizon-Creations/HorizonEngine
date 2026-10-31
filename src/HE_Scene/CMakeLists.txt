add_library(HorizonScene SHARED
	src/GameReplication.cpp
	src/Net/NetGameSession.cpp
	src/Net/SpawnReplicator.cpp
	src/Net/PropertyReplicator.cpp
	src/Net/RpcRouter.cpp
	src/Net/ValueWire.cpp
	src/AntiCheat/AntiCheatService.cpp
	src/AntiCheat/AntiCheatHost.cpp
	src/AntiCheat/AntiCheatTelemetry.cpp
    src/HorizonWorld.cpp
    src/FloatingOrigin.cpp
    src/CellStreamer.cpp
    src/CellSplit.cpp
    src/CellPhysics.cpp
    src/CellRuntime.cpp
    src/CellState.cpp
    src/CellScripts.cpp
    src/LandscapeTile.cpp
    src/LandscapeSource.cpp
    src/LandscapeStore.cpp
    src/LandscapeQuadtree.cpp
    src/LandscapePatchMesh.cpp
    src/LandscapeAtlas.cpp
    src/LandscapeRuntime.cpp
    src/LandscapeGpuLayout.cpp
    src/GameInstanceHost.cpp
    src/PlayerHost.cpp
    src/EntityHost.cpp
    src/AnimatorHost.cpp
    src/MovementSystem.cpp
    src/SceneSerializer.cpp
    src/SceneJsonParse.cpp
    src/EntityVisibility.cpp
    src/EntityActive.cpp
    src/Components/TransformComponent.cpp
    src/TerrainMeshGenerator.cpp
    src/TerrainSystem.cpp
    src/TrailSystem.cpp
    src/TerrainPaint.cpp
    src/TerrainSculpt.cpp
    src/WaterField.cpp
    src/WaterBrush.cpp
    src/WaterLake.cpp
    src/WaterMesh.cpp
    src/WaterTriangulate.cpp
    src/WaterSurface.cpp
    src/WaterQuery.cpp
    src/WaterRipples.cpp
    src/TerrainHeightmap.cpp
    src/TerrainGenerate.cpp
    src/FoliagePaint.cpp
    src/AnimationEval.cpp
    src/AnimationPose.cpp
    src/AnimationIk.cpp
    src/PoseFinalize.cpp
    src/PoseSource.cpp
    src/RootMotion.cpp
    src/RootMotionApply.cpp
    src/AnimationNotify.cpp
    src/AnimationSystem.cpp
    src/AnimationBlendSystem.cpp
    src/AnimationStateMachineSystem.cpp
    src/AnimationPreview.cpp
    src/PropertyAnimationSystem.cpp
    src/SequenceEval.cpp
    src/SequenceSystem.cpp
    src/NavigationSystem.cpp
    src/ParticleSystem.cpp
    src/SplineGeometry.cpp
    src/SplineCurve.cpp
    src/RopeTrailSystem.cpp
    src/LODSystem.cpp
    src/WeatherSystem.cpp
    src/WeatherAudio.cpp
    src/SceneSystems.cpp
    src/EnvironmentPush.cpp
    src/FlyCameraController.cpp
    src/TransformHierarchy.cpp
    src/CameraRigController.cpp
    src/CameraShake.cpp
    src/FoliageSystem.cpp
    src/UISystem.cpp
    src/UIInputSystem.cpp
    src/ScriptContext.cpp
    src/ScriptApi.cpp
    src/EngineApi.cpp
    src/EngineApiCpp.cpp
    src/PlayerSettings.cpp
    src/HcCodegen.cpp
    src/HiddenShell.cpp
    src/AudioEngine.cpp
    src/SteamAudio.cpp
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

# SceneSerializer.cpp outgrew the 2^16 COFF sections of a Debug MSVC object
# (fatal error C1128). CI builds Windows in Release only, so only a local Debug
# build ever saw it.
if(MSVC)
    target_compile_options(HorizonScene PRIVATE /bigobj)
endif()

# Build profile: the physics and transform env overrides are dev-only. Core
# already passes it on; said here too so the target carries the property.
he_target_build_profile(HorizonScene PUBLIC)

target_link_libraries(HorizonScene
    PUBLIC  HorizonCore
    # Gameplay replication (GameReplication) sits on the shared transport.
    # HorizonNet is BELOW the scene layer, so this dependency points downward.
    PUBLIC  HorizonNet
    PRIVATE nlohmann_json::nlohmann_json
            Jolt
            RecastNavigation::Recast
            RecastNavigation::Detour
            # The `db` script group (EngineApi.cpp). PRIVATE: sqlite3.h appears
            # in no header of ours, and the rows hand their results out as JSON
            # text, so nothing above this library ever sees a sqlite3*.
            sqlite3_static
)

# Landscape tiles (LandscapeTile.cpp) are stored with LZ4 or zstd, as HorizonCore does for hpak;
# a build without them writes and reads only the stored form.
if(HE_HAVE_LZ4)
    target_link_libraries(HorizonScene PRIVATE lz4::lz4)
    target_compile_definitions(HorizonScene PRIVATE HE_HAVE_LZ4)
endif()
if(HE_HAVE_ZSTD)
    target_link_libraries(HorizonScene PRIVATE zstd::zstd)
    target_compile_definitions(HorizonScene PRIVATE HE_HAVE_ZSTD)
endif()

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

# ── Steam Audio (root CMakeLists.txt, HE_ENABLE_STEAM_AUDIO) ───────────────────
# PRIVATE on purpose: SteamAudio.h is the same with or without the SDK (the
# answer is SteamAudio::compiledIn()), so no consumer's layout depends on the
# flag. Only phonon.h is needed here; libphonon is opened at run time and sits
# beside the engine library (the editor deploy and the tests' build dir below,
# the game's HorizonEngine/ folder in src/HE_Game). ${CMAKE_DL_LIBS} is dlopen.
target_link_libraries(HorizonScene PRIVATE ${CMAKE_DL_LIBS})
if(HE_HAVE_STEAM_AUDIO)
    get_filename_component(_he_sa_lib_dir "${HE_STEAM_AUDIO_LIB}" DIRECTORY)
    target_compile_definitions(HorizonScene PRIVATE
        HE_HAVE_STEAM_AUDIO
        # Last-resort search directory: a dev tree whose library was not copied.
        "HE_STEAM_AUDIO_LIB_DIR=\"${_he_sa_lib_dir}\"")
    target_include_directories(HorizonScene PRIVATE "${HE_STEAM_AUDIO_INCLUDE_DIR}")
    add_custom_command(TARGET HorizonScene POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HE_STEAM_AUDIO_LIB}" "$<TARGET_FILE_DIR:HorizonScene>"
        VERBATIM)
endif()

he_deploy_dll(HorizonScene)
if(HE_HAVE_STEAM_AUDIO)
    add_custom_command(TARGET HorizonScene POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_EDITOR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HE_STEAM_AUDIO_LIB}" "${DEPLOY_EDITOR}"
        VERBATIM)
endif()
