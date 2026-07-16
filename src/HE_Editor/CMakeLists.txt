set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui)

# ImGui has no CMakeLists — build it as a static lib, editor-only.
# To enable: clone https://github.com/ocornut/imgui (docking branch) and
# copy the root .cpp/.h files plus the backends/ subfolder into
# src/HE_Editor/vendor/imgui/
set(HE_IMGUI_AVAILABLE FALSE)
if(EXISTS "${IMGUI_DIR}/imgui.cpp")
	set(HE_IMGUI_AVAILABLE TRUE)

	set(IMGUI_SOURCES
		${IMGUI_DIR}/imgui.cpp
		${IMGUI_DIR}/imgui_draw.cpp
		${IMGUI_DIR}/imgui_tables.cpp
		${IMGUI_DIR}/imgui_widgets.cpp
		${IMGUI_DIR}/imgui_demo.cpp
		${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
		${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
		${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
	)

	# ── Windows-only backends (D3D11 / D3D12 / Vulkan / Win32) ──────────────
	if(WIN32)
		list(APPEND IMGUI_SOURCES
			${IMGUI_DIR}/backends/imgui_impl_win32.cpp
			${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
			${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
		)
	endif()

	# ── Vulkan backend ────────────────────────────────────────────────────────
	if(DEFINED ENV{VULKAN_SDK} OR Vulkan_FOUND)
		list(APPEND IMGUI_SOURCES
			${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
		)
		set(HE_IMGUI_VULKAN TRUE)
	else()
		set(HE_IMGUI_VULKAN FALSE)
	endif()

	# ── Metal backend (macOS) ─────────────────────────────────────────────────
	if(APPLE)
		list(APPEND IMGUI_SOURCES
			${IMGUI_DIR}/backends/imgui_impl_metal.mm
		)
		# imgui_impl_metal requires ARC
		set_source_files_properties(${IMGUI_DIR}/backends/imgui_impl_metal.mm
			PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
	endif()

	# ImGuizmo (vendored) — gizmo widgets on top of ImGui
	list(APPEND IMGUI_SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/vendor/imguizmo/ImGuizmo.cpp
	)

	# ImGuiColorTextEdit (vendored, MIT) — syntax-highlighting code editor for the
	# script code-editor tab. Uses std::regex (patched from the fork's boost::regex).
	list(APPEND IMGUI_SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/vendor/ImGuiColorTextEdit/TextEditor.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/vendor/ImGuiColorTextEdit/LanguageDefinitions.cpp
	)

	add_library(imgui_editor STATIC ${IMGUI_SOURCES} "EditorUI.h")

	target_include_directories(imgui_editor PUBLIC
		${IMGUI_DIR}
		${IMGUI_DIR}/backends
		${CMAKE_CURRENT_SOURCE_DIR}/vendor/imguizmo
		${CMAKE_CURRENT_SOURCE_DIR}/vendor/ImGuiColorTextEdit
	)
	target_link_libraries(imgui_editor PRIVATE SDL3::SDL3)

	if(WIN32)
		target_link_libraries(imgui_editor PRIVATE d3d11 d3d12 dxgi)
	endif()

	if(APPLE)
		target_link_libraries(imgui_editor PRIVATE
			"-framework Metal" "-framework QuartzCore" "-framework Foundation")
		target_compile_definitions(imgui_editor PUBLIC HE_IMGUI_METAL_ENABLED)
	endif()

	if(HE_IMGUI_VULKAN)
		if(DEFINED ENV{VULKAN_SDK})
			set(Vulkan_INCLUDE_DIR "$ENV{VULKAN_SDK}/Include")
			set(Vulkan_LIBRARY     "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
		endif()
		find_package(Vulkan REQUIRED)
		target_include_directories(imgui_editor PUBLIC ${Vulkan_INCLUDE_DIRS})
		target_link_libraries(imgui_editor PRIVATE Vulkan::Vulkan)
		target_compile_definitions(imgui_editor PUBLIC HE_IMGUI_VULKAN_ENABLED)
	endif()

	message(STATUS "HorizonEditor: Dear ImGui found — building with HE_IMGUI_ENABLED")
else()
	message(STATUS "HorizonEditor: Dear ImGui not found in vendor/ — skipping editor build")
	return()
endif()

add_executable(HorizonEditor WIN32
	main.cpp
	EditorApplication.cpp
	EditorUndo.cpp
	EditorCamera.cpp
	ScriptEditorPanel.cpp
	CppClassEditorPanel.cpp
	MaterialEditorPanel.cpp
	UIEditorPanel.cpp
	LevelScriptPanel.cpp
	InputAssetPanel.cpp
	SkeletalMeshEditorPanel.cpp
	ParticleGraphEditorPanel.cpp
	AnimatorStateMachineEditorPanel.cpp
	HcClassList.cpp
	GraphEditor.cpp
	stb_image_impl.cpp
 "EditorUI.h" "EditorUI.cpp" "ScriptEditorPanel.h" "MaterialEditorPanel.h" "LevelScriptPanel.h" "GameInstancePanel.h" "HorizonCodeClassPanel.h" "HcClassList.h" "SkeletalMeshEditorPanel.h" "ParticleGraphEditorPanel.h" "AnimatorStateMachineEditorPanel.h")

if(APPLE)
	target_sources(HorizonEditor PRIVATE ImGuiMetalBridge.mm ImGuiMetalBridge.h
	                                     MacMenuBar.mm MacMenuBar.h)
	set_source_files_properties(ImGuiMetalBridge.mm MacMenuBar.mm
		PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
	target_link_libraries(HorizonEditor PRIVATE "-framework Cocoa")
endif()

target_include_directories(HorizonEditor PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/vendor
	${CMAKE_CURRENT_SOURCE_DIR}/../HE_Rendering/include
	${CMAKE_CURRENT_SOURCE_DIR}/../HE_Rendering/glad/include
	${CMAKE_CURRENT_SOURCE_DIR}/../HE_Tools/src/FileOps
)

if(TARGET he_materialshader)
	# The editor precompiles material shaders for export (needs the cross-compiler).
	set(HE_EDITOR_LINK_MATSHADER he_materialshader he_shadercompiler)
endif()
target_link_libraries(HorizonEditor PRIVATE
	${HE_EDITOR_LINK_MATSHADER}
	HorizonCore
	HorizonScene
	HorizonRendering
	HorizonTools
	HorizonImporters
	SDL3::SDL3
	glad
)
if(WIN32)
	target_link_libraries(HorizonEditor PRIVATE RendererD3D11 RendererD3D12)
endif()
if(HE_VULKAN_ENABLED)
	target_link_libraries(HorizonEditor PRIVATE RendererVulkan)
	target_compile_definitions(HorizonEditor PRIVATE HE_VULKAN_ENABLED)
endif()
if(APPLE)
	target_link_libraries(HorizonEditor PRIVATE RendererMetal)
endif()

if(HE_IMGUI_AVAILABLE)
	# HE_IMGUI_ENABLED is ONLY defined on HorizonEditor — never on the game or renderer
	target_compile_definitions(HorizonEditor PRIVATE HE_IMGUI_ENABLED)
	target_link_libraries(HorizonEditor PRIVATE imgui_editor)
	target_include_directories(HorizonEditor PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
endif()

# HE_EDITOR enables editor-only code paths (e.g. JSON asset I/O in ContentManager).
# Set PRIVATE on HorizonCore so the flag is baked into the DLL's own translation
# units but NOT propagated to consumers (HorizonGame must not see HE_EDITOR).
target_compile_definitions(HorizonEditor PRIVATE HE_EDITOR)
target_compile_definitions(HorizonCore   PRIVATE HE_EDITOR)

# ── Post-build: deploy HorizonEditor.exe + engine DLLs ────────────────────────
if(NOT DEPLOY_EDITOR OR DEPLOY_EDITOR STREQUAL "")
	set(DEPLOY_EDITOR "$<TARGET_FILE_DIR:HorizonEditor>")
endif()

set(EDITOR_DEPS_DIR "${CMAKE_SOURCE_DIR}/EditorDeps")

# Deploy when the editor exe itself changes
add_custom_command(TARGET HorizonEditor POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_EDITOR}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:HorizonEditor>
		"${DEPLOY_EDITOR}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:SDL3::SDL3>
		"${DEPLOY_EDITOR}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:SDL3::SDL3>
		$<TARGET_FILE_DIR:HorizonEditor>
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:HorizonTools>
		$<TARGET_FILE_DIR:HorizonEditor>
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${EDITOR_DEPS_DIR}"
		"$<TARGET_FILE_DIR:HorizonEditor>"
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${EDITOR_DEPS_DIR}"
		"${DEPLOY_EDITOR}"
	COMMAND ${CMAKE_COMMAND} -E make_directory
		"$<TARGET_FILE_DIR:HorizonEditor>/Images"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${EDITOR_DEPS_DIR}/Images/HC_Logo.png"
		"$<TARGET_FILE_DIR:HorizonEditor>/Images/HC_Logo.png"
	COMMENT "Deploying HorizonEditor.exe to ${DEPLOY_EDITOR}"
	VERBATIM
)

# ── Optional: bundle cmake next to the editor (ship with the engine) ──────────
# So a user who downloads the editor only needs to install a C++ compiler, not
# cmake. We bundle the very cmake that built the engine (CMAKE_COMMAND): the exe
# plus ONLY the data files it needs at runtime — Modules/ (compiler/platform
# detection, required) and Templates/. Everything else in the data dir (Help/ docs,
# ~11-40 MB; editor/shell completions; unrelated share/ neighbours) is dropped.
# HcCodegen::resolveCmake() then finds <app>/cmake/bin/cmake[.exe] first, falling
# back to a system cmake on PATH. Opt-in — turn it ON for the editor build you
# distribute: -DHE_BUNDLE_CMAKE=ON. Verified a Modules+Templates-only bundle
# configures a C++ project.
option(HE_BUNDLE_CMAKE "Bundle a minimal cmake next to the editor so users only need a C++ compiler" OFF)
if(HE_BUNDLE_CMAKE)
	# CMAKE_ROOT is the running cmake's data dir, located reliably across install layouts
	# (official, Homebrew keg, system package). The bundled cmake finds its data relative
	# to its own exe; preserve CMAKE_ROOT's own dir name (upstream: cmake-<major.minor>;
	# Homebrew: cmake) so the relocated exe finds it with the layout it expects.
	if(EXISTS "${CMAKE_ROOT}/Modules")
		get_filename_component(_cm_dataname "${CMAKE_ROOT}" NAME)
		foreach(_dst "$<TARGET_FILE_DIR:HorizonEditor>" "${DEPLOY_EDITOR}")
			add_custom_command(TARGET HorizonEditor POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E make_directory "${_dst}/cmake/bin"
				COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_COMMAND}" "${_dst}/cmake/bin"
				COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_ROOT}/Modules"
					"${_dst}/cmake/share/${_cm_dataname}/Modules"
				COMMENT "Bundling minimal cmake (exe + Modules) into ${_dst}/cmake"
				VERBATIM)
			# Templates/ ships with upstream + Homebrew cmake but can be absent on some
			# distro-packaged cmake installs (Linux). Copy it only if present so the
			# deploy degrades gracefully instead of failing the POST_BUILD.
			if(EXISTS "${CMAKE_ROOT}/Templates")
				add_custom_command(TARGET HorizonEditor POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_ROOT}/Templates"
						"${_dst}/cmake/share/${_cm_dataname}/Templates"
					COMMENT "Bundling cmake Templates into ${_dst}/cmake"
					VERBATIM)
			endif()
		endforeach()
	else()
		message(WARNING "HE_BUNDLE_CMAKE: CMAKE_ROOT data dir not found (${CMAKE_ROOT}) — cmake will not be bundled")
	endif()
endif()

# The editor also links HorizonScene (→ HorizonScene.dll imports python3XY.dll at load),
# so a distributed editor needs the Python runtime + the MSVC C++ runtime next to it,
# exactly like the game does.
he_bundle_python(HorizonEditor "${DEPLOY_EDITOR}")
he_bundle_crt(HorizonEditor "${DEPLOY_EDITOR}")

# Bundle the game runtime INTO the editor package (<editor>/Game) so exporting a game
# works from a downloaded editor: the exporter's findRuntimeBundle() looks for
# <editorBaseDir>/Game/HorizonGame[.exe]. HorizonEditor depends on HorizonGame
# (add_dependencies in the root CMakeLists), so DEPLOY_GAME is fully populated (exe +
# engine DLLs + SDL3 + Python + CRT) by the time this POST_BUILD runs. Copy it wholesale.
add_custom_command(TARGET HorizonEditor POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${DEPLOY_GAME}" "${DEPLOY_EDITOR}/Game"
	COMMENT "Bundling the game runtime into ${DEPLOY_EDITOR}/Game (for in-editor export)"
	VERBATIM)

# he_sdk_config.json: tells the HorizonCode → C++ export codegen where the
# engine headers + libraries live when exporting from a DEVELOPMENT build (the
# dev fallback of plan §8.3 — deployed editors ship a staged SDK/ instead).
# Written beside the editor binary; resolveSdk() reads it at export time.
file(GENERATE
	OUTPUT "$<TARGET_FILE_DIR:HorizonEditor>/he_sdk_config.json"
	CONTENT "{\n  \"includeDirs\": [\"${CMAKE_SOURCE_DIR}/src/HE_Core/include\", \"${glm_SOURCE_DIR}\"],\n  \"libDir\": \"$<TARGET_FILE_DIR:HorizonCore>\"\n}\n")

# The Vulkan backend loads <exe dir>/Shaders/*.spv at runtime, and the D3D12
# backend loads <exe dir>/Shaders/gi_*_hw.cso (DXR RayQuery kernels compiled by
# dxc — see HE_D3D12_GI_SHADERS). Both land in ${CMAKE_BINARY_DIR}/Shaders.
# Without this copy the Vulkan RHI falls over ("shader not found") and D3D12
# silently stays on software GI. Deploy next to the exe and into DEPLOY_EDITOR.
if(HE_VULKAN_ENABLED OR HE_D3D12_GI_SHADERS)
	add_custom_command(TARGET HorizonEditor POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${CMAKE_BINARY_DIR}/Shaders"
			"$<TARGET_FILE_DIR:HorizonEditor>/Shaders"
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${CMAKE_BINARY_DIR}/Shaders"
			"${DEPLOY_EDITOR}/Shaders"
		COMMENT "Deploying Vulkan SPIR-V shaders to the editor"
		VERBATIM
	)
endif()

# ── macOS DMG packaging ────────────────────────────────────────────────────────
# Usage: cmake --build <build_dir> --target dmg
# Output: out/HorizonEditor-<version>-macOS.dmg  (next to the source root)
# Sources the flat deploy in out/deploy/Editor/ which POST_BUILD keeps current.
if(APPLE)
	add_custom_target(dmg
		COMMAND "${CMAKE_SOURCE_DIR}/scripts/package_macos.sh"
		        "${CMAKE_PROJECT_VERSION}"
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		COMMENT "Packaging HorizonEditor ${CMAKE_PROJECT_VERSION} as macOS DMG"
		VERBATIM
	)
	add_dependencies(dmg HorizonEditor)
endif()

# ── Linux: make the editor deploy self-contained ──────────────────────────────
# Bundle the third-party libraries the editor links from system paths (lz4/zstd,
# and libcrypto when not static mbedTLS) next to the binaries, so the tar.gz runs
# without them installed. libpython + SDL3 are already deployed; the $ORIGIN rpath
# finds all of them. Registered last so every editor .so is in place first. macOS is
# NOT included here — its editor ships as a .app built by package_macos.sh, which
# does its own dependency BFS + install-name rewrite (leave that path untouched).
if(UNIX AND NOT APPLE)
	add_custom_command(TARGET HorizonEditor POST_BUILD
		COMMAND "${CMAKE_SOURCE_DIR}/scripts/bundle_native_deps.sh" "${DEPLOY_EDITOR}"
		COMMENT "Bundling native deps into ${DEPLOY_EDITOR} (self-contained editor)"
		VERBATIM)
endif()
