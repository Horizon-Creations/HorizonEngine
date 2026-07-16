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

# Runtime-loaded shader binaries (Vulkan .spv + D3D12 DXR .cso) live in
# ${CMAKE_BINARY_DIR}/Shaders and are loaded from <exe dir>/Shaders at runtime
# — same pattern as the editor deploy (HE_Editor/CMakeLists.txt). Without this
# a packaged game loses Vulkan entirely and D3D12 GI silently drops to the
# software kernels.
if(HE_VULKAN_ENABLED OR HE_D3D12_GI_SHADERS)
	add_custom_command(TARGET HorizonGame POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${CMAKE_BINARY_DIR}/Shaders"
			"${DEPLOY_GAME}/Shaders"
		COMMENT "Deploying runtime shader binaries to the game"
		VERBATIM
	)
endif()

# ── Post-build: bundle the CPython runtime for shipping (Windows) ─────────────
# HorizonScene links Python3::Python, so HorizonScene.dll has python3XY.dll in its
# LOAD-TIME import table — a shipped game needs python3XY.dll next to the executable
# even if it never runs a Python script, plus the stdlib once it does. Without this the
# game runs on the dev box (PATH/registry) but fails on a clean PC ("python314.dll fehlt").
# he_bundle_python (cmake/BundlePython.cmake) copies the DLL(s) + a curated stdlib zip +
# a python3XY._pth (isolated path mode). The Editor's exporter carries these flat files
# next to the exe on a Windows export (see ProjectExporter).
he_bundle_python(HorizonGame "${DEPLOY_GAME}")

# macOS + Linux: also stage the curated stdlib zip + HorizonGame._pth next to the game,
# so a Python-language export runs embedded Python from the bundle on a clean machine.
# The exporter only ships these when the project's language is Python (Windows stages
# its own zip + pythonXY._pth via he_bundle_python above).
he_bundle_python_stdlib(HorizonGame "${DEPLOY_GAME}" "HorizonGame")

# Bundle the MSVC C++ runtime so the game runs on a clean PC without the VC++ redist.
he_bundle_crt(HorizonGame "${DEPLOY_GAME}")

# ── Post-build: make the game runtime SELF-CONTAINED on macOS + Linux ─────────
# The engine dylibs/.so link third-party libraries from the dev's system/Homebrew
# paths (lz4, zstd, openssl, the CPython framework/libpython). A game exported from
# the editor copies DEPLOY_GAME wholesale, so without this it runs on the dev box but
# is broken on any other machine ("Library not loaded: /opt/homebrew/..."). This
# bundles those deps next to the binaries and (macOS) rewrites the links to @rpath +
# ad-hoc re-signs; the game exe already carries an @executable_path/$ORIGIN rpath, so
# they resolve next to it. No-op on Windows (bash-less — Python/CRT handled above).
# MUST run last, after the deploy + he_bundle_python have populated DEPLOY_GAME.
if(UNIX)
    add_custom_command(TARGET HorizonGame POST_BUILD
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/bundle_native_deps.sh" "${DEPLOY_GAME}"
        COMMENT "Bundling native deps into ${DEPLOY_GAME} (self-contained game runtime)"
        VERBATIM)
endif()