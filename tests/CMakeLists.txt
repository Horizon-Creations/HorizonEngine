add_executable(he_tests
    main.cpp
    test_slotmap.cpp
    test_hasset.cpp
    test_contentmanager.cpp
    test_asset_ref_retarget.cpp
    test_asset_ref_scan.cpp
    test_scene_pick.cpp
    test_material_graph.cpp
    test_particle_graph.cpp
    test_animator_state_machine_graph.cpp
    test_scene_serializer.cpp
    test_collab_docsync.cpp
    test_scene_entity_ids.cpp
    test_level_script.cpp
    test_horizoncode_runtime.cpp
    test_horizoncode_compiled.cpp
    test_horizoncode_shortcuts.cpp
    test_graph_editor_keys.cpp
    test_editor_input.cpp
    test_editor_row_widgets.cpp
    test_report_issue.cpp
    test_editor_dock_state.cpp
    test_game_instance.cpp
    test_culling.cpp
    test_gi_bvh.cpp
    test_undo.cpp
    test_editorcamera.cpp
    test_asset_thumbnail_cache.cpp
    test_rendergraph.cpp
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
    test_type_registry.cpp
    test_horizoncode_types.cpp
    test_python_scripting.cpp
    test_physics.cpp
    test_collision_system.cpp
    test_collider.cpp
    test_raycast.cpp
    test_character.cpp
    test_skeletalmeshimporter.cpp
    test_meshimporter.cpp
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
    test_logging.cpp
    test_project_exporter.cpp
    test_export_profiles.cpp
    test_cpp_scaffold.cpp
    test_tutorial.cpp
    test_lod.cpp
    test_weather.cpp
    test_gamelogic_loader.cpp
    test_crypto_x25519.cpp
    test_net_bitstream.cpp
    test_net_loopback.cpp
    test_net_tcp.cpp
    test_net_secure.cpp
    test_net_discovery.cpp
    test_net_directory.cpp
    test_net_lanbeacon.cpp
    test_net_collab.cpp
    test_collab_controller.cpp
    test_collab_undo.cpp
    test_sourcecontrol_probe.cpp
    test_git_status_parse.cpp
    test_git_roundtrip.cpp
    test_game_replication.cpp
    # Culling/sorting/undo/camera/graph are compiled in directly — pulling in the
    # full HorizonRendering/Editor targets would drag GPU backends and ImGui in.
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderExtractor.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderWorld.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/FrustumCuller.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderSorter.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/ScenePick.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/SsaoKernel.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/SkyNoise3D.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/SkyFrameParams.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/LightPacking.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderGraph.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderPass.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/CommandBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/GPUMemoryAllocator.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderResourceManager.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/GiBvh.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorUndo.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorCamera.cpp
    # Compiled in so the editor's real network↔scene bridge is covered
    # end-to-end; it pulls in no ImGui or GPU code.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/CollabController.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/GitController.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/CollabUndo.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/CollabDocSync.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorAssetTypeCache.cpp
    # The tutorial curriculum + its completion predicates are deliberately
    # ImGui-free (TutorialPanel.cpp holds everything that is not), so the step
    # tables can be asserted here without a window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/TutorialSteps.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/AssetThumbnailCache.cpp
    # stb_image's single translation unit. AssetThumbnailCache draws texture
    # tiles from an asset's own pixels and CollabController decodes a chosen
    # profile picture; both call into it, so the tests need it linked too.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/stb_image_impl.cpp
    # The presence bar is almost entirely ImGui, and almost entirely guarded by
    # HE_IMGUI_ENABLED — which the tests do not define. What survives is
    # PlaceMarker: pure projection geometry, and the piece where a wrong Y flip
    # or a mishandled behind-the-camera case hides best.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/CollabPresenceBar.cpp
    # The graph quick-spawn key table is deliberately plain data and ImGui-free
    # (HcGraphHost turns a letter into an ImGuiKey), so its invariants can be
    # asserted here without a window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/HcGraphShortcuts.cpp
    # Report-Issue: the dialog is ImGui, but the part that has to be right — the
    # markdown body and the URL that has to stay under GitHub's request-line
    # limit — is pure and compiles without HE_IMGUI_ENABLED.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/ReportIssueDialog.cpp
    # "Is this panel docked?" — ImGui-only by construction (see the header),
    # so the DockIsActive-vs-DockNode invariant it rests on can be asserted.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorDockState.cpp
    # The node canvas + ImGui's CORE only (no backend, no GPU): ImGui runs
    # headless given a context and a display size, which is what lets
    # test_graph_editor_keys.cpp actually press the graph shortcuts. The editor
    # target stays out of this — it is the backends that drag the GPU in, not
    # ImGui itself.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/GraphEditor.cpp
    # Labelled rows + wrapped hints: ImGui-only by construction (see the file's
    # header), so the panel-width layout invariant can be asserted headless.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorRowWidgets.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imgui/imgui_widgets.cpp
)

target_include_directories(he_tests PRIVATE
    vendor
    # The source-control module's private src/, so the log-redaction helper can
    # be covered directly — it is the last defence against a token reaching a
    # log file, so it is worth testing rather than trusting.
    ${CMAKE_SOURCE_DIR}/src/HE_SourceControl/src
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/include
    ${CMAKE_SOURCE_DIR}/src/HE_Editor
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imgui
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
# hpak encryption tests run whenever a crypto backend (system OpenSSL or the
# fetched mbedTLS fallback) is compiled in — the round-trips go through
# HorizonCore's backend-agnostic Hpak::aesGcm* API.
if(HE_HAVE_OPENSSL OR HE_HAVE_MBEDTLS)
    target_compile_definitions(he_tests PRIVATE HE_HAVE_CRYPTO)
endif()
# astcenc for the ASTC cook round-trip test (encode → decode → compare).
if(HE_HAVE_ASTCENC)
    target_link_libraries(he_tests PRIVATE ${HE_ASTCENC_TARGET})
    target_include_directories(he_tests PRIVATE ${astcenc_SOURCE_DIR}/Source)
    target_compile_definitions(he_tests PRIVATE HE_HAVE_ASTCENC)
endif()
# BC7 / BC3 cook tests only need the HE_HAVE_* flag to gate on (the encoders live
# inside HorizonCore; the tests just check the cooked format/mip/size structurally).
if(HE_ENABLE_BC7)
    target_compile_definitions(he_tests PRIVATE HE_HAVE_BC7ENC)
endif()
if(HE_ENABLE_STB_DXT)
    target_compile_definitions(he_tests PRIVATE HE_HAVE_STB_DXT)
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
    latent_flow enums structs GameInstance gi_caller engine_events
)
set(HCGEN_SOURCES ${HCGEN_DIR}/hc_registry.cpp)
# The generated headers carry the classes themselves and are declared outputs
# too: deleting one has to re-run the generator, not fail the compile.
set(HCGEN_HEADERS ${HCGEN_DIR}/hc_registry.h ${HCGEN_DIR}/hcgen_types.h
                  ${HCGEN_DIR}/hcgen.h)
# One header per Struct/Enum the fixtures define (fixtures.h registerTypes) —
# a class includes only the definitions it uses, not the umbrella.
foreach(t E_FixMood E_FixDup S_FixInner S_FixStats)
    list(APPEND HCGEN_HEADERS ${HCGEN_DIR}/hcgen_type_${t}.h)
endforeach()
foreach(c ${HCGEN_CLASSES})
    list(APPEND HCGEN_SOURCES ${HCGEN_DIR}/hcgen_C_${c}.cpp)
    list(APPEND HCGEN_HEADERS ${HCGEN_DIR}/hcgen_C_${c}.h)
endforeach()
add_custom_command(OUTPUT ${HCGEN_SOURCES} ${HCGEN_HEADERS}
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
# he_tests compiles the SAME generated headers, so without hidden visibility the
# two binaries would coalesce their inline statics — and the loader test's
# dlclose would pull them out from under the in-process parity tests. The shipped
# project generateCMakeLists() writes does exactly this too.
set_target_properties(test_hcgen PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(test_hcgen PRIVATE HorizonCore)
target_include_directories(test_hcgen PRIVATE ${HCGEN_DIR})
add_dependencies(he_tests test_hcgen)
target_sources(he_tests PRIVATE test_hccompiled_loader.cpp)
target_compile_definitions(he_tests PRIVATE
    HE_TEST_HCGEN_LIB="$<TARGET_FILE:test_hcgen>")

# ─── Process-runner fixture ────────────────────────────────────────────────────
# A real child process, because every bug HE::Proc exists to avoid is invisible
# to a mock: exit codes mangled into wait statuses, streams merged, pipes
# deadlocking on volume, arguments corrupted by quoting, grandchildren surviving
# a kill. The test binary learns its path the same way the hcgen fixture above
# does, so nothing depends on the working directory.
add_executable(he_proc_child proc_child_main.cpp)
# Needs the project standard explicitly: without it MSVC compiled this at an
# older level where std::string::data() still returns const char*, which
# CreateProcessA rejects.
target_compile_features(he_proc_child PRIVATE cxx_std_20)
add_dependencies(he_tests he_proc_child)
target_sources(he_tests PRIVATE test_process_runner.cpp)
target_compile_definitions(he_tests PRIVATE
    HE_TEST_PROC_CHILD="$<TARGET_FILE:he_proc_child>")

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
    HorizonNet
    HorizonSourceControl
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
            $<TARGET_FILE:HorizonNet>   $<TARGET_FILE_DIR:he_tests>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:HorizonSourceControl> $<TARGET_FILE_DIR:he_tests>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:HorizonScene> $<TARGET_FILE_DIR:he_tests>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:HorizonTools> $<TARGET_FILE_DIR:he_tests>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3-shared>  $<TARGET_FILE_DIR:he_tests>
        VERBATIM
    )
endif()
