add_executable(HorizonGame WIN32
	src/main.cpp
	src/GameApplication.cpp
	src/EmbeddedPakKey.cpp
)

target_include_directories(HorizonGame PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(HorizonGame PRIVATE
	HorizonCore
	HorizonScene
	HorizonRendering
	SDL3::SDL3
)

# Add @executable_path to rpath so a copied binary finds its dylibs alongside it
if(APPLE)
    set_target_properties(HorizonGame PROPERTIES
        BUILD_RPATH "@executable_path"
    )
endif()

if(DEFINED HE_PROJECT_SECRET)
	target_compile_definitions(HorizonGame
		PRIVATE HE_PROJECT_SECRET="${HE_PROJECT_SECRET}"
	)
endif()

# ── Post-build: deploy HorizonGame.exe + runtime DLLs ────────────────────────
# Uses DEPLOY_GAME from the root CMakeLists.txt.
# The engine DLLs copied here are the same binaries linked by HorizonGame
# (built WITHOUT HE_EDITOR). Do NOT copy them from the Editor build output.
add_custom_command(TARGET HorizonGame POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_GAME}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:HorizonGame>
		"${DEPLOY_GAME}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:SDL3::SDL3>
		"${DEPLOY_GAME}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:HorizonCore>
		"${DEPLOY_GAME}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:HorizonRendering>
		"${DEPLOY_GAME}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:HorizonScene>
		"${DEPLOY_GAME}"
	COMMENT "Deploying HorizonGame.exe to ${DEPLOY_GAME}"
	VERBATIM
)

# ── Post-build: bundle the CPython runtime for shipping (Windows) ─────────────
# HorizonScene links Python3::Python, so HorizonScene.dll has python3XY.dll in its
# LOAD-TIME import table — a shipped game needs python3XY.dll next to the executable
# even if it never runs a Python script, plus the stdlib once it does. Without this the
# game runs on the dev box (PATH/registry) but fails on a clean PC ("python314.dll fehlt").
# he_bundle_python (cmake/BundlePython.cmake) copies the DLL(s) + a curated stdlib zip +
# a python3XY._pth (isolated path mode). The Editor's exporter carries these flat files
# next to the exe on a Windows export (see ProjectExporter).
he_bundle_python(HorizonGame "${DEPLOY_GAME}")