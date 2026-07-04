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
	stb_image_impl.cpp
 "EditorUI.h" "EditorUI.cpp" "ScriptEditorPanel.h")

if(APPLE)
	target_sources(HorizonEditor PRIVATE ImGuiMetalBridge.mm ImGuiMetalBridge.h)
	set_source_files_properties(ImGuiMetalBridge.mm
		PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

target_include_directories(HorizonEditor PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/vendor
	${CMAKE_CURRENT_SOURCE_DIR}/../HE_Rendering/include
	${CMAKE_CURRENT_SOURCE_DIR}/../HE_Rendering/glad/include
	${CMAKE_CURRENT_SOURCE_DIR}/../HE_Tools/src/FileOps
)

target_link_libraries(HorizonEditor PRIVATE
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

# The Vulkan backend loads <exe dir>/Shaders/*.spv at runtime. They are compiled
# to ${CMAKE_BINARY_DIR}/Shaders by the VulkanShaders target (a dependency of
# RendererVulkan). Without this copy the editor crashes/falls over on the Vulkan
# RHI ("shader not found"). Deploy them next to the exe and into DEPLOY_EDITOR.
if(HE_VULKAN_ENABLED)
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
