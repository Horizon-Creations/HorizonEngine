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

# The game picks its backend from config.json ("GameBackend"), so it has to know
# which ones this runtime can actually create — RendererFactory throws for one
# that was never compiled in. Same define the editor takes for its backend list.
if(HE_VULKAN_ENABLED)
	target_compile_definitions(HorizonGame PRIVATE HE_VULKAN_ENABLED)
endif()

# ── Development-only hotkeys ─────────────────────────────────────────────────
# Debug keys that must NOT be live in a shipped game (today: V = VSync toggle —
# a player has no settings menu to undo it with, and a stray V would silently
# change their present mode). Release/MinSizeRel are what the CI packages and
# what the exporter ships as the game runtime, so they compile these out.
target_compile_definitions(HorizonGame PRIVATE
	$<$<CONFIG:Debug,RelWithDebInfo>:HE_GAME_DEV_HOTKEYS>
)

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
	# HorizonScene links HorizonNet (GameReplication), so a shipped game needs
	# it beside the exe or it will not start at all.
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:HorizonNet>
		"${DEPLOY_GAME}"
	COMMENT "Deploying HorizonGame.exe to ${DEPLOY_GAME}"
	VERBATIM
)

# The Python plugin, when the engine has one. It goes into the runtime TEMPLATE
# unconditionally — the template must be able to serve any project — while the
# exporter decides per project whether it reaches the finished game (see
# ProjectExporter). Unlike the libraries above, this one is never linked, only
# loaded by name at runtime, so a game shipped without it simply has no Python.
if(HE_HAVE_PYTHON)
	add_custom_command(TARGET HorizonGame POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:HorizonPython>
			"${DEPLOY_GAME}"
		COMMENT "Deploying the Python plugin to ${DEPLOY_GAME}"
		VERBATIM)
	add_dependencies(HorizonGame HorizonPython)
endif()

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
# python3XY.dll is a load-time dependency of the PLUGIN (HorizonPython), not of
# HorizonScene — that was the point of splitting it out. It still has to sit next
# to the executable for a project that uses Python, plus the stdlib; without it
# the game runs on the dev box (PATH/registry) and fails on a clean PC
# ("python314.dll fehlt"). A project that uses no Python ships neither, and the
# exporter is where that decision is made.
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