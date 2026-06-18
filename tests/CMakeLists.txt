add_executable(he_tests
    main.cpp
    test_slotmap.cpp
    test_hasset.cpp
    test_contentmanager.cpp
    test_scene_serializer.cpp
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
    test_physics.cpp
    test_collider.cpp
    test_raycast.cpp
    test_character.cpp
    test_skeletalmeshimporter.cpp
    # Culling/sorting/undo/camera/graph are compiled in directly — pulling in the
    # full HorizonRendering/Editor targets would drag GPU backends and ImGui in.
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

target_link_libraries(he_tests PRIVATE
    HorizonCore
    HorizonScene
    HorizonImporters
    nlohmann_json::nlohmann_json
)

add_test(NAME he_tests COMMAND he_tests)
