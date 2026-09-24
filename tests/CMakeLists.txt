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
    test_hc_function_rename.cpp
    test_hc_rename_scope.cpp
    test_graph_editor_keys.cpp
    test_preview_pick.cpp
    test_gizmo_pick.cpp
    test_editor_input.cpp
    test_editor_row_widgets.cpp
    test_editor_theme.cpp
    test_report_issue.cpp
    test_notification_store.cpp
    # Node highlighting + console→node: hits fade, reveals are two one-shots.
    test_hc_exec_trace.cpp
    test_hc_breakpoint_play_path.cpp
    # The watch window's rows: a stopped run's event argument, frames, locals,
    # variables and outputs, spelled as the window shows them.
    test_hc_watch.cpp
    # The first error of a play session opens the console: the decision,
    # run through sessions (header-only, PlayErrorReveal.h).
    test_play_error_reveal.cpp
    test_editor_dock_state.cpp
    test_scene_autosave.cpp
    test_game_instance.cpp
    test_culling.cpp
    test_world_preview_grid.cpp
    test_gi_bvh.cpp
    test_undo.cpp
    test_editor_selection.cpp
    test_editor_marquee.cpp
    test_editor_multi_edit.cpp
    test_outliner_filter.cpp
    # The eye, the padlock and the sibling order behind each Outliner row.
    test_outliner_row_actions.cpp
    # Hide / Isolate / Show All / Group / Ungroup behind the viewport's
    # right-click menu: the sun stays, the group's members do not jump.
    test_viewport_actions.cpp
    # …and the row itself, driven headless: the two buttons sit ON the tree
    # node (AllowOverlap), and whether a click reaches them or selects the row
    # is a question only a pointer can answer.
    test_outliner_ui.cpp
    test_inspector_ui.cpp
    # The Landscape panel's Foliage brush, driven the same way: mode well,
    # Grow/Erase well, and a drag over a top-down viewport into the mask.
    test_terrain_tools_ui.cpp
    # The Audio Mixer, driven the same way: strips from a real ProjectData, a
    # click on M reaching the device-less AudioEngine as a mute, S as a solo.
    test_audio_mixer_ui.cpp
    # The Undo History window, driven the same way: rows from a real EditorUndo
    # over a real world, a click on a row rewinding the world in one restore.
    test_undo_history_ui.cpp
    # The Watch window, driven the same way: sections and rows from a real
    # runtime stopped at a breakpoint, Go to Node raising the reveal.
    test_hc_watch_ui.cpp
    test_project_settings_ui.cpp
    # The recovery dialog, driven the same way: three answers, three callbacks,
    # and no dialog at all without an offer.
    test_scene_recovery_ui.cpp
    test_editorcamera.cpp
    test_camera_bookmarks.cpp
    test_asset_thumbnail_cache.cpp
    test_rendergraph.cpp
    # The GL sky as the one source for Vulkan/D3D11/D3D12: prelude coverage,
    # SkyFrameParams layout, cross-compile, and on Windows FXC + WARP.
    test_sky_shader.cpp
    test_jobsystem.cpp
    test_terrain.cpp
    # Greyscale heightmap → heights: the pixel path, the texture and file
    # wrappers (8/16-bit PGM, .r16), orientation and the down-sampling box.
    test_terrain_heightmap.cpp
    # Was on disk since the foliage layer landed but never in this list, so the
    # scatter had no running test until the density mask needed one.
    test_foliage.cpp
    test_debug_draw.cpp
    test_profiler.cpp
    test_profiler_stats.cpp
    test_profiler_layout.cpp
    test_audio.cpp
    test_rendermanager.cpp
    test_prefab.cpp
    test_inspector_prefab_keys.cpp
    test_inputmapping.cpp
    test_input_gamepad.cpp
    test_eventbus.cpp
    test_scripting.cpp
    test_scripting_binding.cpp
    test_engine_api.cpp
    test_time_control.cpp
    test_type_registry.cpp
    test_horizoncode_types.cpp
    test_hc_fallback_report.cpp
    test_horizoncode_containers.cpp
    test_horizoncode_reroute.cpp
    test_python_scripting.cpp
    test_physics.cpp
    test_collision_system.cpp
    test_entity_host.cpp
    test_player_host.cpp
    test_animator_host.cpp
    test_movement.cpp
    test_hc_inheritance.cpp
    test_collider.cpp
    test_raycast.cpp
    test_character.cpp
    test_camera_rig.cpp
    test_skeletalmeshimporter.cpp
    test_meshimporter.cpp
    test_assimpimport.cpp
    test_gltf_material_import.cpp
    test_skeletalmeshcomponent.cpp
    test_animationsystem.cpp
    test_root_motion.cpp
    test_animation_notify.cpp
    test_animation_layers.cpp
    test_blend_space.cpp
    test_sequencer_timeline.cpp
    test_animation_ik.cpp
    test_navigation.cpp
    test_particles.cpp
    test_ui.cpp
    test_ui_widgets.cpp
    # Its own file so it gets its own PROCESS: the shared font atlas is baked
    # once, and this is the one run that bakes it in the regular weight.
    test_ui_font_weight.cpp
    test_app_icon.cpp
    test_hidden_window.cpp
    test_document_types.cpp
    test_autostart.cpp
    test_software_raster.cpp
    test_app_todo.cpp
    test_app_preview_outliner.cpp
    test_ui_theme.cpp
    test_postfx.cpp
    test_hpak.cpp
    test_async_streaming.cpp
    test_crash_handler.cpp
    test_logging.cpp
    test_project_exporter.cpp
    test_export_profiles.cpp
    test_project_settings.cpp
    test_export_backend.cpp
    test_cpp_scaffold.cpp
    test_tutorial.cpp
    test_third_person_template.cpp
    test_docs_library.cpp
    test_editor_help.cpp
    test_hc_node_docs.cpp
    # Editor UI rendered on the CPU and looked at — see ImGuiSoftwareRaster.h.
    test_ui_shot.cpp
    ImGuiSoftwareRaster.cpp
    test_lod.cpp
    test_rope_trail.cpp
    test_editor_icons.cpp
    # A click on a light's icon selects the light: the Scene window's picking
    # rule against a snapshot the real RenderExtractor produced.
    test_viewport_pick.cpp
    # …and the gizmo moves it: EditorTransformGizmo over a mesh-less entity,
    # driven headless the way test_gizmo_pick.cpp drives ImGuizmo.
    test_viewport_gizmo.cpp
    # The shortcut table: overrides round-trip through the config string,
    # conflicts are per scope, a chord fires only with its exact modifiers.
    test_editor_shortcuts.cpp
    # …and the Preferences page over it, driven headless: click a chord,
    # press keys, read the binding back out of the pinned config.json.
    test_shortcuts_ui.cpp
    test_weather.cpp
    test_gamelogic_loader.cpp
    test_gamelogic_services.cpp
    test_gamelogic_build.cpp
    test_crypto_x25519.cpp
    test_net_bitstream.cpp
    test_net_loopback.cpp
    test_net_tcp.cpp
    # The gameplay transport over real localhost UDP sockets, with an injected
    # clock and the transport's own drop hook for loss under the reliability.
    test_net_udp.cpp
    # The deterministic bad-network decorator itself: does what its numbers say,
    # the same way every run, on its own simulated clock.
    test_net_lossy.cpp
    test_net_secure.cpp
    test_net_discovery.cpp
    test_net_directory.cpp
    test_net_lanbeacon.cpp
    test_net_collab.cpp
    test_collab_controller.cpp
    test_collab_undo.cpp
    test_editor_commands.cpp
    test_mcp_bridge.cpp
    test_mcp_tools_entity.cpp
    test_mcp_tools_asset.cpp
    test_mcp_tools_scene.cpp
    test_mcp_tools_terrain.cpp
    test_mcp_tools_widget.cpp
    test_mcp_tools_input.cpp
    test_mcp_tools_material.cpp
    test_mcp_tools_prefab.cpp
    test_mcp_tools_type.cpp
    test_mcp_tools_particle.cpp
    test_mcp_tools_animator.cpp
    test_mcp_tools_clip.cpp
    test_mcp_tools_build.cpp
    test_mcp_tools_settings.cpp
    test_mcp_tools_batch.cpp
    test_mcp_tools_screenshot.cpp
    test_mcp_camera_gizmos.cpp
    test_mcp_hc.cpp
    test_mcp_api.cpp
    test_mcp_client_setup.cpp
    test_mcp_claude_probe.cpp
    test_sourcecontrol_probe.cpp
    test_git_status_parse.cpp
    test_git_roundtrip.cpp
    test_game_replication.cpp
    # The first real gameplay session: two and three NetGameSessions against
    # each other over LossyTransport, headless and on a simulated clock.
    test_net_game_session.cpp
    # Replicated variables over the same hostile fabric: the wire format on its
    # own, then values reaching a client that is losing and reordering.
    test_net_property_replication.cpp
    test_net_rpc.cpp
    test_anticheat.cpp
    test_integrity.cpp
    # Culling/sorting/undo/camera/graph are compiled in directly — pulling in the
    # full HorizonRendering/Editor targets would drag GPU backends and ImGui in.
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderExtractor.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderWorld.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/FrustumCuller.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/OcclusionCuller.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderSorter.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/ScenePick.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/SsaoKernel.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/SkyNoise3D.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/SkyFrameParams.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/LightPacking.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderGraph.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderPass.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/MaterialScalars.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/CommandBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/GPUMemoryAllocator.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/RenderResourceManager.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/GiBvh.cpp
    # The software UI rasterizer, compiled in for the same reason as the culler:
    # it is quads-in-pixels-out with no SDL, no window and no IRenderer, so what
    # the UI style vocabulary ACTUALLY draws is answerable headless — the one
    # thing about "Schicht 0" that used to need a GPU and a pair of eyes.
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/Backends/Software/SoftwareRaster.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorUndo.cpp
    # The editor's selection set: handles in, handles out, no ImGui — so what
    # a Ctrl-click, a Shift-range and a peer's delete do to it is answerable here.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorSelection.cpp
    # The rubber-band rule: which boxes a drawn frame encloses, given a camera.
    # Matrices in, yes/no out — the part of "drag a frame around these" that a
    # test can ask without a window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorMarquee.cpp
    # The viewport's right-click verbs (hide / isolate / show all / group /
    # ungroup): a registry and a selection in, touched entities out — no ImGui.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/ViewportActions.cpp
    # Multi-selection editing: which leaves of the active entity's component
    # state changed, and how they land on the other members. JSON in, JSON
    # out, plus a real world for the write-back — no widgets involved.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorMultiEdit.cpp
    # The Outliner's search and type dropdown: which rows of a depth-first
    # hierarchy stay, and as what (hit or the dimmed path to one). Rows in,
    # verdicts out, plus a real registry for the type table — no ImGui.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/OutlinerFilter.cpp
    # The single door every scene change goes through — the UI's structural
    # commands, a peer's edit and, later, an external MCP client. It is
    # deliberately free of ImGui and of EditorApplication (which is NOT in this
    # binary), which is what lets the question the gateway exists to answer —
    # "does undo put the world back exactly as it was, whoever asked" — be
    # answered here rather than by hand in front of a window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorCommands.cpp
    # The MCP bridge and its tool registry. Compiled in for the same reason as
    # the gateway above: the interesting questions — does an unauthenticated
    # caller get dropped, does the endpoint file carry the token at 0600, does a
    # fifth connection get refused — are about a real socket and a real file, and
    # neither needs a window. EditorApplication is NOT in this binary, which is
    # exactly why the bridge takes its editor state as hooks.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolRegistry.cpp
    # What "Add to Claude" will run before it runs it: the CLI search, the
    # argument vector, and the reading of the result. In here because the button
    # is a promise that a click works, and the parts of that promise which can be
    # checked without Claude Code installed on the build machine are exactly the
    # parts that are wrong most easily — a missing -s user, a path that stopped
    # being absolute, an exit code read as success.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpClientSetup.cpp
    # And the reading of `claude mcp get`, which is where the Tool Status page's
    # "Connected" row gets its verdict. The CLI exits ZERO whether the server
    # connected or failed to, so the whole verdict hangs on parsing text — the
    # kind of thing that is right until a wording changes, and the kind of thing
    # a test can hold to the transcripts that were actually measured.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpClaudeProbe.cpp
    # The entity tools. In here for the reason they were written this way at all:
    # they take an EditorCommands reference and nothing else from the editor, so
    # "does entity_set_transform actually move the entity, and does undo put it
    # back" is a question this binary can answer.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsEntity.cpp
    # The HorizonCode authoring tools, in here for the same reason: they take the
    # documents as hooks and a graph is a value, so "does a connect by pin LABEL
    # land on the pin the client meant" and "does an edit through these tools
    # come out of the collab diff as exactly one delta" are both answerable
    # against a HorizonCode::Graph on the stack, with no panel and no window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsHc.cpp
    # The engine API as tools. In here because the file is generated FROM a
    # table: "does every row of HE::api::registry() turn into a tool with a
    # legal name and a schema that matches its signature" is a question about
    # two data structures, and the answer must not depend on a window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsApi.cpp
    # The asset tools, and this is the one place their central claim can be put
    # to the test: a real ContentManager over a temporary content root answers
    # "did the file actually move", "did the material that named it follow" and
    # "does a path with '..' in it get out of the root" — none of which a running
    # editor could be asked without a project and a window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsAsset.cpp
    # The scene tools, and this binary is where their central claim is provable
    # at all: a real HorizonWorld plus a real SceneSerializer over a temporary
    # content root answers "did what scene_save wrote come back out of
    # scene_open with the same entities under the same uuids" — the round trip
    # a running editor could only be asked about with a project and a window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsScene.cpp
    # The terrain tools. In here because their central claim is only checkable
    # against a real component: the brush runs on a copy and the result goes
    # through the gateway as a component patch, so "did the ground move where it
    # was asked, does undo put it back, and did the never-serialised runtime
    # fields survive the round trip through the scene JSON" are three questions
    # about a HorizonWorld on the stack — and the third one is invisible in a
    # running editor until a hundred paint calls have leaked a hundred textures.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsTerrain.cpp
    # The widget tools. In here for the reason the asset tools are: their
    # central claim — an edit lands in the OPEN Designer tab and nowhere else,
    # and on disk only when no tab holds the asset — is two code paths that
    # cannot both be exercised in a running editor, because the second one needs
    # the tab to not exist. A UIWidgetTree on the stack plays the tab.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsWidget.cpp
    # The input tools, and this binary is the only place their central claim is
    # provable: "would the RUNTIME read this binding". A real ContentManager over
    # a temporary content root plus a real InputMapping answers it — the file the
    # tool wrote goes straight into applyInputMappingContext, which is the
    # function that silently skips a name it cannot parse. No round trip through
    # our own decoder can catch that, and a running editor could not be asked
    # without a project, a window and a keyboard.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsInput.cpp
    # The material tools, and the same argument as the input ones: their central
    # claim is "does the value survive the codegen the Material Editor re-runs on
    # every edit", and only a real ContentManager over a temporary content root
    # plus the real HE::generateFragment can answer it.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsMaterial.cpp
    # The prefab tools sit on both halves of the editor at once — the gateway for
    # the placement, a real ContentManager over a temporary content root for the
    # asset — and the claim worth testing spans both: does a placement really come
    # back out of the undo stack, and does the file a placement was made from
    # still round-trip through SceneSerializer afterwards.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsPrefab.cpp
    # The type tools, and the same argument as the input ones: their claim is
    # "the definition the rest of the engine now sees is the one that was asked
    # for", and only a real ContentManager over a temporary content root plus the
    # real HE::TypeRegistry round-trip can answer it.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsType.cpp
    # The particle and animation tools. Their claim is "the emitter the
    # simulation gets afterwards is the one that was asked for" — which only the
    # real HE::ParticleGraph / HE::AnimatorStateMachineGraph round-trip through a
    # real ContentManager can answer, so the sources are compiled in rather than
    # stubbed.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsParticle.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsAnimator.cpp
    # Same reason once more: the claim is that the clip the simulation reads
    # afterwards carries the events that were asked for, and only a real
    # AnimationClipAsset round-trip through a real ContentManager answers it.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsClip.cpp
    # The build tools. In here because the two things they actually decide can
    # be decided without a build machine: WHICH profile a run is made from (and
    # that a one-off override never reaches the stored one), and whether the one
    # shared Build window is free. The export itself is a worker thread behind a
    # hook, and a test plays that hook.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsBuild.cpp
    # The settings tools and the table underneath them, and this is the binary
    # where their central claim is provable: the project half runs against a REAL
    # ProjectManager over a temporary directory, so "did the manifest actually
    # keep it" is answered by loading the project again rather than by reading
    # back the struct we just wrote.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsSettings.cpp
    # The batch dispatcher: runs other tools through the registry, so the test
    # binary needs only the registry and whatever tools the test registers.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsBatch.cpp
    # The screenshot tool: the renderer is a hook, so "does a fake frame come
    # back as a PNG the decoder reads pixel-for-pixel, as an image block, at the
    # size asked, from the camera asked" is answerable without a GPU.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolsScreenshot.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorSettingsCatalog.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpToolCommon.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/AssetStubWriter.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorCamera.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/CameraBookmarks.cpp
    # Compiled in so the editor's real network↔scene bridge is covered
    # end-to-end; it pulls in no ImGui or GPU code.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/CollabController.cpp
    # CollabController posts to it (a refused request, a peer that could not
    # apply a delete), so the tests cannot link without it — and asserting what
    # lands in it is how those posts are covered at all.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/NotificationStore.cpp
    # The runtime's exec trace as the editor keeps it (test_hc_exec_trace.cpp).
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/HcExecTrace.cpp
    # The stopped run as watch rows (test_hc_watch.cpp) — ImGui-free like the trace.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/HcWatch.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/GitController.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/CollabUndo.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/CollabDocSync.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorAssetTypeCache.cpp
    # The Input Mapping panel's model: the decoded asset, its JSON codec and the
    # rule for which row a pressed input lands in. ImGui-free by construction
    # (the panel does the drawing and the device polling), so "did that press
    # bind to the row it was aimed at" is answerable without a window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/InputMappingModel.cpp
    # The tutorial curriculum + its completion predicates are deliberately
    # ImGui-free (TutorialPanel.cpp holds everything that is not), so the step
    # tables can be asserted here without a window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/TutorialSteps.cpp
    # The in-editor manual: the bundle model and its search, ImGui-free for the
    # same reason (DocsPanel.cpp is the skin). The test loads the SHIPPED bundle,
    # so a docs conversion that dropped the tables fails here rather than in a
    # blank panel nobody opens until after the release.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/DocsLibrary.cpp
    # The help table behind the editor's tooltips. Plain data, no ImGui — which
    # is what lets the test walk it and check that every topic it points at
    # still exists in the documentation bundle. It is also linked because
    # EditorRowWidgets.cpp (below) looks its rows up in it.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorHelp.cpp
    # Per-node documentation: plain data over the engine's own registries,
    # so the coverage test can walk both against each other.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/HcNodeDocs.cpp
    # The generated node-reference page — built from the registries, so the
    # test can check the manual has an entry per callable thing.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/HcNodeReference.cpp
    # The editor reference, generated from the help table — the test walks
    # both against each other.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorReference.cpp
    # The guides — the third generated part of the manual, and the one a reader
    # starts in. Same reason as the two above: DocsPanel installs them, so the
    # test target has to carry them or it does not link.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorGuides.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/Guides/GuidesFirstSteps.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/Guides/GuidesCharacter.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/Guides/GuidesWorld.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/Guides/GuidesShipping.cpp
    # The documentation reader itself, so test_ui_shot.cpp can render the real
    # panel rather than a stand-in. It is ImGui plus the library above; the two
    # files after it are what it leans on (window placement, the panel outline).
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/DocsPanel.cpp
    # The toolbar look — wells and segmented cells. The reader's language
    # switcher is one, and borrowing the primitives is what keeps it from being
    # a second copy of the palette. ImGui-only by construction, so it compiles
    # here without HE_IMGUI_ENABLED (see the guard at the top of its header).
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorToolbar.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorWidgets.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/HcEditorUtil.cpp
    # Which HorizonCode classes an export shipped interpreted instead of
    # compiled — the model behind the build window's warning band. ImGui-free
    # by construction, so "does a fallback reach the list with its reason" is
    # answerable without a window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/HcFallbackReport.cpp
    # Renaming a HorizonCode member across assets. Deliberately free of ImGui and
    # of the content system, which is what lets the whole decision — whose call is
    # this, may it be rewritten — be asserted here instead of clicked through.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/HcRename.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/PanelSpotlight.cpp
    # The mesh tabs' material-slot list. Takes a ContentManager and the widget
    # that draws a slot, never AppContext — which is what lets the UI-shot test
    # draw it over an in-memory mesh and assert what a slot edit writes back.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/MeshMaterialSlots.cpp
    # The Sequencer's strip — tracks, ruler, scrubbing — over a
    # PropertyAnimClipAsset and a View, never AppContext, for the same reason:
    # "does a drag on the ruler land the playhead where the ruler says" is a
    # question about arithmetic and hit tests, not about a window.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/SequencerTimeline.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/AssetThumbnailCache.cpp
    # The solo autosave's timing and file rules: when a recovery snapshot is
    # written, that the write is temp+rename with the manifest last, and what
    # a previous run's leftover becomes at the next start. Clock, dirty flag
    # and revision are handed in, so it runs on a fake clock, not a stopwatch.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/SceneAutosave.cpp
    # stb_image's single translation unit. AssetThumbnailCache draws texture
    # tiles from an asset's own pixels and CollabController decodes a chosen
    # profile picture; both call into it, so the tests need it linked too.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/stb_image_impl.cpp
    # The presence bar is almost entirely ImGui, and almost entirely guarded by
    # HE_IMGUI_ENABLED — which the tests do not define. What survives is
    # PlaceMarker: pure projection geometry, and the piece where a wrong Y flip
    # or a mishandled behind-the-camera case hides best.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/CollabPresenceBar.cpp
    # The MCP clients' camera gizmos: the frustum half is pure geometry into a
    # DebugDrawBuffer (does it point where the camera looks, does it scale with
    # the field of view, does every client get its own colour); the tag half
    # is ImGui and compiles away here, like the presence bar's.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/McpCameraGizmos.cpp
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
    # Where a click in an offscreen 3D pane lands. Pure projection + slab math
    # by construction (no ImGui, no entt, no ContentManager) precisely so the
    # class Viewport's picking can be asserted — that pane is the one thing in
    # the editor that cannot be screenshotted headless.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/PreviewPick.cpp
    # The Scene window's own click rule (mesh before terrain, icon quads like
    # any mesh), against a RenderWorld snapshot. ImGui-free for the same reason.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/ViewportPick.cpp
    # The Details panel's label→scene-key table behind the prefab "(changed
    # here)" marker. Plain data in its own file so the test can hold every key
    # against the serializer's list without dragging the panel in.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/InspectorPrefabKeys.cpp
    # Labelled rows + wrapped hints: ImGui-only by construction (see the file's
    # header), so the panel-width layout invariant can be asserted headless.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorRowWidgets.cpp
    # The ImGui style, for the same reason: it is ImGui-only by construction, so
    # "did the theme actually assign every colour slot" is answerable headless.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorTheme.cpp
    # The transform gizmo, for the same reason as GraphEditor above: ImGuizmo is
    # ImGui and maths, nothing else, so where its handles can be GRABBED is
    # answerable headless — the one property of the viewport that no screenshot
    # dump can show.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imguizmo/ImGuizmo.cpp
    # The editor's wrapper around it — the latch-during-drag and write-back
    # rules — so "a drag on a light-only entity moves the light" is a test and
    # not a hope. Its header is guarded by HE_IMGUI_ENABLED (the flag the editor
    # target alone defines); the per-file definition below turns it on for this
    # one source and its test, and for nothing else in the binary.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorTransformGizmo.cpp
    # The shortcut table, for test_editor_shortcuts.cpp: same per-file
    # HE_IMGUI_ENABLED (ImGuiKeyChord is its currency).
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorShortcuts.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/ShortcutsPage.cpp
    # The World Outliner, for test_outliner_ui.cpp: it needs a real AppContext
    # (built there) and ImGui, and nothing else in this binary. Same per-file
    # HE_IMGUI_ENABLED as the gizmo — AppContext's layout depends on it, so the
    # panel and its test are the only two sources here that see that layout.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/OutlinerPanel.cpp
    # The Details panel, for test_inspector_ui.cpp: same AppContext, same
    # per-file HE_IMGUI_ENABLED. Everything it reaches for (the widgets, the
    # help table, the multi-edit diff, the HorizonCode class listing, the
    # Landscape tools) is already in this binary for the other panels.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/InspectorPanel.cpp
    # The Landscape tools, for test_terrain_tools_ui.cpp: same AppContext, same
    # per-file HE_IMGUI_ENABLED. Everything they call (TerrainPaint/FoliagePaint
    # in HorizonScene, RenderWorld, the toolbar, the widgets) is already here.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/TerrainTools.cpp
    # The Audio Mixer window, for test_audio_mixer_ui.cpp: same AppContext, same
    # per-file HE_IMGUI_ENABLED. It reads ProjectData (HorizonTools, linked
    # below) and AudioEngine (HorizonScene), both already here.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/AudioMixerPanel.cpp
    # The Undo History window, for test_undo_history_ui.cpp: same AppContext,
    # same per-file HE_IMGUI_ENABLED. It reads EditorUndo (already here).
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/UndoHistoryPanel.cpp
    # The Watch window, for test_hc_watch_ui.cpp: same AppContext, same per-file
    # HE_IMGUI_ENABLED. It reads HcExecTrace and HcWatch (both already here).
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/HcWatchPanel.cpp
    # The recovery dialog, for test_scene_recovery_ui.cpp: same AppContext, same
    # per-file HE_IMGUI_ENABLED. It reads four callbacks and a RecoveryInfo.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/SceneRecoveryDialog.cpp
    # The Project Settings tab, for test_project_settings_ui.cpp: same
    # AppContext, same per-file HE_IMGUI_ENABLED. It reads ProjectData and
    # writes Config/ProjectSettings.json through the ProjectManager.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/ProjectSettingsPanel.cpp
    # The icon/splash rows the settings tab draws (shared with the Export dialog).
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/AppMetadataRows.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imgui/imgui_widgets.cpp
    # InputText over a std::string — the editor panels compiled in here use it.
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imgui/misc/cpp/imgui_stdlib.cpp
)

set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorTransformGizmo.cpp
    test_viewport_gizmo.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/EditorShortcuts.cpp
    test_editor_shortcuts.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/ShortcutsPage.cpp
    test_shortcuts_ui.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/OutlinerPanel.cpp
    test_outliner_ui.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/InspectorPanel.cpp
    test_inspector_ui.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/TerrainTools.cpp
    test_terrain_tools_ui.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/AudioMixerPanel.cpp
    test_audio_mixer_ui.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/UndoHistoryPanel.cpp
    test_undo_history_ui.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/HcWatchPanel.cpp
    test_hc_watch_ui.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/SceneRecoveryDialog.cpp
    test_scene_recovery_ui.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/ProjectSettingsPanel.cpp
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/AppMetadataRows.cpp
    test_project_settings_ui.cpp
    PROPERTIES COMPILE_DEFINITIONS HE_IMGUI_ENABLED)

target_include_directories(he_tests PRIVATE
    vendor
    # The source-control module's private src/, so the log-redaction helper can
    # be covered directly — it is the last defence against a token reaching a
    # log file, so it is worth testing rather than trusting.
    ${CMAKE_SOURCE_DIR}/src/HE_SourceControl/src
    ${CMAKE_SOURCE_DIR}/src/HE_Rendering/include
    ${CMAKE_SOURCE_DIR}/src/HE_Editor
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imgui
    ${CMAKE_SOURCE_DIR}/src/HE_Editor/vendor/imguizmo
)
# The HorizonRendering sources above are compiled straight into the test exe
# instead of linking the DLL. Define HE_RENDERING_BUILD_DLL so HE_RENDERING_API
# resolves to dllexport (definitions are local to this exe) rather than dllimport,
# which would emit unresolved __imp_ references for those same locally-built symbols.
target_compile_definitions(he_tests PRIVATE HE_RENDERING_BUILD_DLL)
# The documentation bundle the editor ships (EditorDeps is copied next to the
# executable at build time). Handed to the test as a path rather than found by
# walking up from the working directory, which differs between ctest, an IDE
# runner and a packaged test binary.
target_compile_definitions(he_tests PRIVATE
    HE_DOCS_BUNDLE_PATH="${CMAKE_SOURCE_DIR}/EditorDeps/Docs/he-docs.json")
# The deployed game runtime the export half of the Wave-1 acceptance copies into
# its packaged build (docs/he-apps-plan.md). Handed over as a path, like the docs
# bundle above: a configure without the deploy step simply has none, and the test
# says so and skips rather than failing.
target_compile_definitions(he_tests PRIVATE
    HE_TEST_GAME_RUNTIME_DIR="${DEPLOY_GAME}")
# The editor's own font, for the UI screenshots (test_ui_shot.cpp). Without it
# they still render, in ImGui's default font — but half of what a UI shot shows
# is line height and spacing, and those belong to the font.
target_compile_definitions(he_tests PRIVATE
    HE_EDITOR_DEPS_DIR="${CMAKE_SOURCE_DIR}/EditorDeps")
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
# Compile the Python scripting tests only when CPython is embedded. They dlopen
# the HorizonPython plugin through its public ABI, so he_tests still needs no
# libpython link of its own — but the plugin has to exist, and the test has to
# be told where it is: the engine finds it next to HorizonScene, and a test
# executable does not live there.
if(HE_HAVE_PYTHON)
    # The path the direct-ABI test loads the plugin from MUST be the same FILE
    # the engine's own loader resolves, or the plugin gets mapped twice. On
    # Windows the loader finds the copy beside he_tests.exe (HorizonScene.dll
    # is relocated there, see the POST_BUILD block below), and LoadLibrary
    # dedupes by path — two different paths mean two module instances, and the
    # second one dies in PyImport_AppendInittab() after Py_Initialize()
    # (CPython is process-global; the plugin must exist once per process).
    # On Unix TARGET_FILE already IS the beside-HorizonScene file, and dlopen
    # dedupes by inode anyway.
    if(WIN32)
        target_compile_definitions(he_tests PRIVATE
            HE_HAVE_PYTHON
            HE_PYTHON_PLUGIN_PATH="$<TARGET_FILE_DIR:he_tests>/$<TARGET_FILE_NAME:HorizonPython>")
    else()
        target_compile_definitions(he_tests PRIVATE
            HE_HAVE_PYTHON
            HE_PYTHON_PLUGIN_PATH="$<TARGET_FILE:HorizonPython>")
    endif()
    add_dependencies(he_tests HorizonPython)
endif()

# ─── HorizonCode → C++ parity fixtures (plan §10.1) ───────────────────────────
# hc_codegen builds the shared fixture graphs (fixtures/hcodegen/fixtures.h) and
# emits the compiled classes at BUILD time; they are compiled INTO he_tests so
# no compiler is needed at test runtime.
set(HCGEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/hcgen_fixtures)
set(HCGEN_CLASSES
    flow_branch_sequence coerce_matrix math_ops bitwise_ops vector_ops variables functions_basic
    functions_recursive foreach_arrays events_multi widget_props limits_smoke
    functions_locals engine_pure_multiout engine_exec_cached animator_sync
    ref_target refs_objects dispatch_owner dispatch_listener dispatch_sink
    latent_flow enums structs GameInstance gi_caller engine_events
    cast_target casts inherit_base inherit_derived
    inherit_novars_base inherit_novars input_actions containers reroutes
    cheat_event
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

# ─── Crash-handler fixture ─────────────────────────────────────────────────────
# A real child that installs CrashHandler and then really dies. What the handler
# does is only observable from outside the process it runs in: the .crash file
# it leaves, and the exit status the OS reports afterwards. A separate binary
# rather than a mode of he_proc_child, which is deliberately dependency-free —
# this one has to link HorizonCore. On Windows it lands next to he_tests, where
# the POST_BUILD step below has already copied the engine DLLs.
add_executable(he_crash_child crash_child_main.cpp)
target_link_libraries(he_crash_child PRIVATE HorizonCore)
target_compile_features(he_crash_child PRIVATE cxx_std_20)
add_dependencies(he_tests he_crash_child)
target_compile_definitions(he_tests PRIVATE
    HE_TEST_CRASH_CHILD="$<TARGET_FILE:he_crash_child>")

# ─── Native game-logic fixture ─────────────────────────────────────────────────
# A real shared library implementing IGameLogic, loaded at runtime by
# test_gamelogic_loader.cpp to exercise GameLogicLoader end to end.
add_library(test_gamelogic SHARED fixtures/test_gamelogic.cpp)
target_link_libraries(test_gamelogic PRIVATE HorizonCore HorizonScene)
add_dependencies(he_tests test_gamelogic)
target_compile_definitions(he_tests PRIVATE
    HE_TEST_GAMELOGIC_LIB="$<TARGET_FILE:test_gamelogic>")

# ─── Engine-services fixture: a game module on the REAL scaffold diet ─────────
# One include directory, NO link libraries and C++17, exactly what
# CppScaffold::cmakeLists gives a generated C++ game project. The constraint is
# the test: a <HorizonGameServices.h> that ever reached for glm, HE_Scene or an
# engine library — or for a language feature the scaffold's standard does not
# have, which a defaulted operator== already nearly was — would fail to compile
# here instead of failing in a user's project. test_gamelogic_services.cpp loads
# it and drives the injected physics, input and content tables through it.
# The engine checkout, for test_gamelogic_build.cpp: it runs the editor's own
# game-logic build against a scaffold CMakeLists, and that file wants exactly
# one thing — HORIZON_ENGINE_DIR. In a deployed editor the same value comes out
# of engineRootFromSdk(); here the build system already knows it.
target_compile_definitions(he_tests PRIVATE
    HE_TEST_ENGINE_ROOT="${CMAKE_SOURCE_DIR}")

add_library(test_gamelogic_services SHARED fixtures/test_gamelogic_services.cpp)
target_include_directories(test_gamelogic_services PRIVATE
    "${CMAKE_SOURCE_DIR}/src/HE_Core/include")
target_compile_features(test_gamelogic_services PRIVATE cxx_std_17)
add_dependencies(he_tests test_gamelogic_services)
target_compile_definitions(he_tests PRIVATE
    HE_TEST_GAMELOGIC_SERVICES_LIB="$<TARGET_FILE:test_gamelogic_services>")

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
target_link_libraries(he_tests PRIVATE he_materialshader)
# test_sky_shader.cpp calls he::shaderc directly, as the Vulkan/D3D sky passes do
# (he_materialshader links it PRIVATE, so its header does not come along).
target_link_libraries(he_tests PRIVATE he_shadercompiler)
# The TARGET is always there now (A3b Teil 2 — without glslang it is built against the
# stub), so "can I link it" stopped being the question. The question these cases ask is
# "can this build actually cross-compile", and only HE_HAVE_SHADERC answers that: a
# -DHE_ENABLE_SHADERC=OFF build compiles every one of them into a guaranteed failure.
if(HE_HAVE_SHADERC)
    target_compile_definitions(he_tests PRIVATE HE_TESTS_HAVE_SHADERC=1)
    # On Windows the material-graph tests go one step further than emission and
    # run the emitted HLSL through the REAL D3DCompile (vs_5_0 / ps_5_0), the
    # call D3D11Renderer and D3D12Renderer make for every graph material. FXC
    # is the only judge of register limits (X4509) and intrinsic spelling that
    # SPIRV-Cross's "ok" says nothing about. d3dcompiler_47.dll ships with
    # Windows, so no SDK component beyond the headers is needed. d3d12 + dxgi:
    # the Thema-56 cases build the material root signature and a real PSO on a
    # WARP device (CreateGraphicsPipelineState is the only judge of "does the
    # root signature cover every register the bytecode binds"); WARP ships with
    # Windows too, so a runner without a GPU still gets the verdict. d3d11: the
    # Thema-57 case draws a wired Landscape Layer Blend on a D3D11 WARP device
    # through the same per-draw bindings D3D11Renderer uses (t14 + s0) and reads
    # the pixel back — the only D3D11 execution this tree gets without a GPU.
    if(WIN32)
        target_link_libraries(he_tests PRIVATE d3dcompiler d3d12 d3d11 dxgi)
    endif()
endif()

# The D3D11/D3D12 shader managers (Thema 62): compiled straight into the test
# exe like the other HorizonRendering sources above — RendererD3D11/D3D12 would
# drag the whole backends in, and neither renderer instantiates these classes
# anyway. The test drives them on a WARP device and reads COM refcounts across
# cleanup(). Independent of HE_HAVE_SHADERC: D3DCompile needs no glslang. Added
# BEFORE the per-file ctest registration below so the test gets its own entry.
if(WIN32)
    target_sources(he_tests PRIVATE
        test_d3d_shader_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/Backends/D3D11/D3D11ShaderManager.cpp
        ${CMAKE_SOURCE_DIR}/src/HE_Rendering/src/Backends/D3D12/D3D12ShaderManager.cpp)
    target_link_libraries(he_tests PRIVATE d3dcompiler d3d11)
endif()

# One ctest entry per test SOURCE FILE, not one for the whole binary.
#
# The suite is wait-bound, not CPU-bound: it spawns real git processes, kills
# child processes, shakes hands over loopback sockets and polls with sleeps.
# Measured on an M-series Mac: one serial process needs ~430 s (CI: 442 s),
# while the test bodies themselves sum to ~31 s and the same 1893 cases spread
# over eight processes finish in 11 s. Splitting the registration is what lets
# `ctest -j` turn that waiting into overlap; nothing about the tests changes.
#
# Each entry gets its OWN temp directory. Several tests write fixed file names
# into temp_directory_path(), which is harmless in sequence and a race the
# moment two processes do it at once.
#
# The root has to sit OUTSIDE this repository, not under the build directory:
# the git tests assert that a plain directory is NOT a repository, and
# findRepoRoot walks upwards — a temp directory inside the checkout finds
# HorizonEngine's own .git and the assertion flips.
if(WIN32)
    set(_he_tmp_root "$ENV{TEMP}/he_tests")
elseif(DEFINED ENV{TMPDIR})
    set(_he_tmp_root "$ENV{TMPDIR}/he_tests")
else()
    set(_he_tmp_root "/tmp/he_tests")
endif()
# Windows hands back backslashes, which CMake would read as escapes inside the
# ENVIRONMENT property.
file(TO_CMAKE_PATH "${_he_tmp_root}" _he_tmp_root)

get_target_property(_he_test_sources he_tests SOURCES)
foreach(_src IN LISTS _he_test_sources)
    get_filename_component(_name "${_src}" NAME)
    # main.cpp and proc_child_main.cpp carry no test cases (the latter is the
    # helper binary's entry point, compiled in but never a test on its own).
    if(NOT _name MATCHES "^test_.*\\.cpp$")
        continue()
    endif()
    get_filename_component(_stem "${_name}" NAME_WE)
    set(_tmp "${_he_tmp_root}/${_stem}")
    file(MAKE_DIRECTORY "${_tmp}")
    add_test(NAME ${_stem} COMMAND he_tests --source-file=*${_name})
    # TMPDIR is what std::filesystem::temp_directory_path() reads on macOS and
    # Linux, TMP/TEMP on Windows. Set all three so the isolation holds anywhere.
    set_tests_properties(${_stem} PROPERTIES
        ENVIRONMENT "TMPDIR=${_tmp};TMP=${_tmp};TEMP=${_tmp}")
endforeach()

# ── "Does every control explain itself?" ─────────────────────────────────────
# A missing tooltip is invisible: nothing fails to build and nothing fails a
# test, the user just hovers and gets nothing. The audit walks the panels'
# SOURCE for labelled widgets and holds them against the help table, which is
# why it is a script rather than a doctest — C++ cannot read its own call sites.
# It fails when an area gains an uncovered control, against the baseline
# recorded in the script itself.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME editor_help_audit
             COMMAND ${Python3_EXECUTABLE}
                     "${CMAKE_SOURCE_DIR}/scripts/editor_help_audit.py" --check)
endif()

# ── The stdio MCP shim ───────────────────────────────────────────────────────
# scripts/he_mcp.py is what an MCP client actually starts; the editor's bridge
# is on the far side of a socket. It is Python, so it is tested in Python — and
# as a SUBPROCESS over real pipes, because the three ways it can break a client
# (something other than JSON-RPC on stdout, an answer to a notification, a
# missing flush) are invisible from inside the process. The editor side is a
# fake TCP server in the test, so this needs no editor, no GUI and no build.
if(Python3_Interpreter_FOUND)
    add_test(NAME he_mcp_shim
             COMMAND ${Python3_EXECUTABLE}
                     "${CMAKE_SOURCE_DIR}/tests/test_he_mcp.py")
endif()

# ── "How heavy is what we actually ship?" ────────────────────────────────────
# docs/he-apps-plan.md A3 wrote a size table into the plan and got it wrong,
# because the numbers came from a tree that was not a Release build. That is why
# this is a test with a threshold rather than a paragraph: the MEASURING POINT is
# part of the claim, and it is printed with every run.
#
# It measures the deploy tree the exporter copies verbatim into a packaged build.
# Absent (a configure without the deploy step, a fresh checkout) is a SKIP, not a
# failure — exit code 2, declared below — or every such build would go red for
# having nothing to weigh.
#
# The build type is handed over for the same reason the path is printed: the
# thresholds were measured on a RELEASE tree and mean nothing on a Debug one,
# which is several times the size. Anything else reports and skips.
#
# One test per runtime flavour (docs/he-apps-plan.md A3b), because the three are
# different artefacts with different limits and a single number would either let
# the app runtimes grow unwatched or fail the game runtime on day one. Only the
# game flavour is deployed by an ordinary build; the two app ones come from
# scripts/build_runtimes.py, so on a normal machine — and in CI — they skip. That
# is deliberate: the test exists so that a tree which HAS built them is measured,
# and it costs nothing in one that has not.
if(Python3_Interpreter_FOUND)
    set(_runtime_size_dirs
        runtime_size              "${DEPLOY_DIR}/Game"        game
        runtime_size_app_advanced "${DEPLOY_DIR}/AppAdvanced" app-advanced
        runtime_size_app_basic    "${DEPLOY_DIR}/AppBasic"    app-basic)
    list(LENGTH _runtime_size_dirs _rs_len)
    math(EXPR _rs_last "${_rs_len} / 3 - 1")
    foreach(_i RANGE ${_rs_last})
        math(EXPR _n "${_i} * 3")
        math(EXPR _d "${_n} + 1")
        math(EXPR _f "${_n} + 2")
        list(GET _runtime_size_dirs ${_n} _rs_name)
        list(GET _runtime_size_dirs ${_d} _rs_dir)
        list(GET _runtime_size_dirs ${_f} _rs_flavor)
        # --flavor is passed explicitly rather than left to the directory name:
        # DEPLOY_DIR is a cache PATH a user may point anywhere, and a runtime
        # weighed against the wrong flavour's threshold is worse than none.
        add_test(NAME ${_rs_name}
                 COMMAND ${Python3_EXECUTABLE}
                         "${CMAKE_SOURCE_DIR}/scripts/runtime_size.py"
                         "${_rs_dir}" --check "--flavor=${_rs_flavor}"
                         "--build-type=$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,${CMAKE_BUILD_TYPE}>")
        set_tests_properties(${_rs_name} PROPERTIES SKIP_RETURN_CODE 2)
    endforeach()
endif()

# The socket tests bind ephemeral ports, but they also probe the LAN (SSDP
# multicast) and hand sessions between two live endpoints. Letting two of them
# run at the same instant makes a failure depend on the scheduler, which is the
# one thing a test may never do. They share a lock, so they serialise against
# each other while the other hundred files still run in parallel.
set(_he_net_tests test_net_tcp test_net_udp test_net_secure test_net_discovery test_net_collab
                  test_net_directory test_collab_controller test_net_game_session)
foreach(_t IN LISTS _he_net_tests)
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES RESOURCE_LOCK he_net)
    endif()
endforeach()

# The two-endpoint collaboration test is the one that measures time rather than
# just using it: it pumps both ends until a deadline and fails if the handshake
# has not landed by then. Under a full -j load it missed that deadline once in
# four runs — a scheduler-dependent failure, which is worse than a slow test.
# RUN_SERIAL gives it the machine for the four seconds it needs.
if(TEST test_collab_controller)
    set_tests_properties(test_collab_controller PROPERTIES RUN_SERIAL TRUE)
endif()

# Same story, one case deep: every gameplay-session test but one runs on an
# injected transport and a simulated clock, and that one brings up host() and
# joinDirect() over real localhost UDP against a five-second deadline. Under a
# full -j load a deadline is a race against the scheduler unless the test has
# the machine.
if(TEST test_net_game_session)
    set_tests_properties(test_net_game_session PROPERTIES RUN_SERIAL TRUE)
endif()

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
    # The Python plugin must sit NEXT TO HorizonScene.dll — and on Windows the
    # copy above relocates HorizonScene beside the test exe, so the loader's
    # thisLibraryDir() points HERE, not at src/HE_Scene. Without this copy the
    # ScriptContext-path Python tests fail with "No Python plugin at …" while
    # Linux/macOS pass (their rpath loads HorizonScene from its build dir,
    # where the plugin already is).
    if(HE_HAVE_PYTHON)
        add_custom_command(TARGET he_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:HorizonPython> $<TARGET_FILE_DIR:he_tests>
            VERBATIM
        )
    endif()
endif()
