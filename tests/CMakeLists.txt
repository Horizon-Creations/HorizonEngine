add_executable(he_tests
    main.cpp
    test_slotmap.cpp
    test_hasset.cpp
    test_contentmanager.cpp
    test_material_graph.cpp
    test_scene_serializer.cpp
    test_level_script.cpp
    test_horizoncode_runtime.cpp
    test_horizoncode_compiled.cpp
    test_game_instance.cpp
    test_culling.cpp
    test_undo.cpp
    test_editorcamera.cpp
    test_rendergraph.cpp
    test_ref.cpp
    test_jobsystem.cpp
    test_terrain.cpp
    test_debug_draw.cpp
    test_profiler.cpp
    test_audio.cpp
    test_rendermanager.cpp
    test_prefab.cpp
    test_inputmapping.cpp
    test_eventbus.cpp
    test_scripting.cpp
    test_scripting_binding.cpp
    test_engine_api.cpp
    test_python_scripting.cpp
    test_physics.cpp
    test_collision_system.cpp
    test_collider.cpp
    test_raycast.cpp
    test_character.cpp
    test_skeletalmeshimporter.cpp
    test_skeletalmeshcomponent.cpp
    test_animationsystem.cpp
    test_navigation.cpp
    test_particles.cpp
    test_ui.cpp
    test_ui_widgets.cpp
    test_postfx.cpp
    test_hpak.cpp
    test_async_streaming.cpp
    test_crash_handler.cpp
    test_project_exporter.cpp
    test_export_profiles.cpp
    test_lod.cpp
    test_weather.cpp
    test_gamelogic_loader.cpp
    # Culling/sorting/undo/camera/graph are compiled in directly — pulling in the
    # full HorizonRendering/Editor targets would drag GPU backends and ImGui in.
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderExtractor.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderWorld.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/FrustumCuller.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderSorter.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderGraph.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderPass.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/CommandBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/GPUMemoryAllocator.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderResourceManager.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorUndo.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorCamera.cpp
)

target_include_directories(he_tests PRIVATE
    vendor
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/include
    ${CMAKE_SOURCE_DIR}/src/HE_Editor
)
# The HorizonRendering sources above are compiled straight into the test exe
# instead of linking the DLL. Define HE_RENDERING_BUILD_DLL so HE_RENDERING_API
# resolves to dllexport (definitions are local to this exe) rather than dllimport,
# which would emit unresolved __imp_ references for those same locally-built symbols.
target_compile_definitions(he_tests PRIVATE HE_RENDERING_BUILD_DLL)
# Mirror the codec availability macros so hpak tests can assert compression
# actually happened (the writer silently falls back to Store when unavailable).
if(HE_HAVE_LZ4)
    target_compile_definitions(he_tests PRIVATE HE_HAVE_LZ4)
endif()
if(HE_HAVE_ZSTD)
    target_compile_definitions(he_tests PRIVATE HE_HAVE_ZSTD)
endif()
if(HE_HAVE_OPENSSL)
    target_compile_definitions(he_tests PRIVATE HE_HAVE_OPENSSL)
endif()
# astcenc for the ASTC cook round-trip test (encode → decode → compare).
if(HE_HAVE_ASTCENC)
    target_link_libraries(he_tests PRIVATE astcenc-native-static)
    target_include_directories(he_tests PRIVATE ${astcenc_SOURCE_DIR}/Source)
    target_compile_definitions(he_tests PRIVATE HE_HAVE_ASTCENC)
endif()
# Compile the Python scripting tests only when CPython is embedded. The tests use
# only PyScriptBackend's public API (linked via HorizonScene), so no direct
# libpython link is needed here.
if(HE_HAVE_PYTHON)
    target_compile_definitions(he_tests PRIVATE HE_HAVE_PYTHON)
endif()

# ─── HorizonCode → C++ parity fixtures (plan §10.1) ───────────────────────────
# hc_codegen builds the shared fixture graphs (fixtures/hcodegen/fixtures.h) and
# emits the compiled classes at BUILD time; they are compiled INTO he_tests so
# no compiler is needed at test runtime.
set(HCGEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/hcgen_fixtures)
set(HCGEN_CLASSES
    flow_branch_sequence coerce_matrix math_ops variables functions_basic
    functions_recursive foreach_arrays events_multi widget_props limits_smoke
    functions_locals engine_pure_multiout engine_exec_cached
    ref_target refs_objects dispatch_owner dispatch_listener dispatch_sink
)
set(HCGEN_SOURCES ${HCGEN_DIR}/hc_registry.cpp)
foreach(c ${HCGEN_CLASSES})
    list(APPEND HCGEN_SOURCES ${HCGEN_DIR}/hcgen_C_${c}.cpp)
endforeach()
add_custom_command(OUTPUT ${HCGEN_SOURCES}
    COMMAND hc_codegen --fixtures --out ${HCGEN_DIR}
    DEPENDS hc_codegen ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hcodegen/fixtures.h
    COMMENT "hc_codegen: generating HorizonCode parity fixtures")
target_sources(he_tests PRIVATE ${HCGEN_SOURCES} test_horizoncode_codegen.cpp)
target_include_directories(he_tests PRIVATE ${HCGEN_DIR} fixtures/hcodegen)

# The same generated sources also become a REAL shared library with the C
# manifest export — test_hccompiled_loader.cpp loads it through
# CompiledClassTable, the exact path the shipped game takes (plan §10.4).
add_library(test_hcgen SHARED ${HCGEN_SOURCES})
target_compile_definitions(test_hcgen PRIVATE HCGEN_BUILD_DYLIB)
target_link_libraries(test_hcgen PRIVATE HorizonCore)
target_include_directories(test_hcgen PRIVATE ${HCGEN_DIR})
add_dependencies(he_tests test_hcgen)
target_sources(he_tests PRIVATE test_hccompiled_loader.cpp)
target_compile_definitions(he_tests PRIVATE
    HE_TEST_HCGEN_LIB="$<TARGET_FILE:test_hcgen>")

# ─── Native game-logic fixture ─────────────────────────────────────────────────
# A real shared library implementing IGameLogic, loaded at runtime by
# test_gamelogic_loader.cpp to exercise GameLogicLoader end to end.
add_library(test_gamelogic SHARED fixtures/test_gamelogic.cpp)
target_link_libraries(test_gamelogic PRIVATE HorizonCore HorizonScene)
add_dependencies(he_tests test_gamelogic)
target_compile_definitions(he_tests PRIVATE
    HE_TEST_GAMELOGIC_LIB="$<TARGET_FILE:test_gamelogic>")

# Real HorizonGame executable path, so the macOS .app-bundle test can exercise
# codesign against a genuine Mach-O (fake bytes cannot be signed).
if(TARGET HorizonGame)
    add_dependencies(he_tests HorizonGame)
    target_compile_definitions(he_tests PRIVATE
        HE_TEST_GAME_EXE="$<TARGET_FILE:HorizonGame>")
endif()

target_link_libraries(he_tests PRIVATE
    HorizonCore
    HorizonScene
    HorizonImporters
    HorizonTools
    nlohmann_json::nlohmann_json
)

# Shared material shader layer (CPU-only: glslang + SPIRV-Cross under the hood) — lets
# the material-graph tests validate that GENERATED GLSL cross-compiles through the REAL
# path (standard vertex + lighting preamble + fragment) for the Metal + GL targets.
if(TARGET he_materialshader)
    target_link_libraries(he_tests PRIVATE he_materialshader)
    target_compile_definitions(he_tests PRIVATE HE_TESTS_HAVE_SHADERC=1)
endif()

add_test(NAME he_tests COMMAND he_tests)

# On Windows the test binary is in tests/ but the engine DLLs are in per-target
# subdirectories. There is no rpath equivalent, so copy the DLLs next to the exe
# via POST_BUILD (mirrors he_deploy_dll). SDL3.dll is needed transitively by
# HorizonCore.dll even though tests don't use it directly.
if(WIN32)
    add_custom_command(TARGET he_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:HorizonCore>  $<TARGET_FILE_DIR:he_tests>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:HorizonScene> $<TARGET_FILE_DIR:he_tests>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:HorizonTools> $<TARGET_FILE_DIR:he_tests>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3-shared>  $<TARGET_FILE_DIR:he_tests>
        VERBATIM
    )
endif()
