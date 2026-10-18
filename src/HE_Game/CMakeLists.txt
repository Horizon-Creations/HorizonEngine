# The engine SDK builds the same game from these sources outside this tree:
# horizon_add_game() in cmake/sdk/HorizonEngineGame.cmake mirrors the target
# setup below (sources, .mm flags, frameworks, rpath, HE_VULKAN_ENABLED, dev
# hotkeys, HE_GAME_BUILD_CONFIG). A change here belongs there too; ctest
# sdk_example_project builds that copy.
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
    # The application's menu bar in the system bar (plan A6). ObjC++, so it is
    # compiled here and nowhere else — the same reason the editor's MacMenuBar
    # sits in HE_Editor: Cocoa must not reach HorizonCore, which he_tests,
    # hc_codegen, widget_gen and the Windows CI all link.
    target_sources(HorizonGame PRIVATE src/AppMacMenu.mm src/AppMacMenu.h
                                       src/AppNotify.mm  src/AppNotify.h)
    set_source_files_properties(src/AppMacMenu.mm src/AppNotify.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # UserNotifications for the banners — its own framework, not part of Cocoa.
    target_link_libraries(HorizonGame PRIVATE "-framework Cocoa"
                                              "-framework UserNotifications")
endif()

# The game picks its backend from config.json ("GameBackend"), so it has to know
# which ones this runtime can actually create — RendererFactory throws for one
# that was never compiled in. Same define the editor takes for its backend list.
if(HE_VULKAN_ENABLED)
	target_compile_definitions(HorizonGame PRIVATE HE_VULKAN_ENABLED)
endif()

# ── Build profile ────────────────────────────────────────────────────────────
# A release tree (HE_BUILD_PROFILE=release, what an SDK export compiles) has no
# V/F10 code at all, whatever the config or the hcfg say (HE_FEATURE_DEV_HOTKEYS
# in Platform/BuildProfile.h). Everything below about HE_GAME_DEV_HOTKEYS is the
# dev tree's runtime choice, i.e. the prebuilt runtime the exporter copies.
he_target_build_profile(HorizonGame PRIVATE)

# ── Development-only hotkeys ─────────────────────────────────────────────────
# Debug keys that must NOT be live in a shipped game (today: V = VSync toggle —
# a player has no settings menu to undo it with, and a stray V would silently
# change their present mode). Release/MinSizeRel are what the CI packages and
# what the exporter ships as the game runtime, so they compile these out.
target_compile_definitions(HorizonGame PRIVATE
	$<$<CONFIG:Debug,RelWithDebInfo>:HE_GAME_DEV_HOTKEYS>
)
# …or in EVERY configuration, for looking at a Release-speed game with the F10 stats
# card: -DHE_GAME_DEV_HOTKEYS=ON. Never for a build that is shipped to players — the
# exporter copies whatever runtime it finds, so a game exported from such a build
# carries the keys too. The startup log says which kind of build it is
# ("development hotkeys compiled in" / "not compiled in").
option(HE_GAME_DEV_HOTKEYS "Compile the development hotkeys (V = VSync, F10 = stats card) into HorizonGame in every configuration" OFF)
if(HE_GAME_DEV_HOTKEYS)
	if(HE_BUILD_PROFILE STREQUAL "release")
		message(WARNING "HE_GAME_DEV_HOTKEYS is ignored in a release tree "
		                "(HE_BUILD_PROFILE=release): the hotkeys are not compiled in")
	endif()
	target_compile_definitions(HorizonGame PRIVATE HE_GAME_DEV_HOTKEYS)
endif()

# ── Compiled-in project configuration (Thema 211 step 4) ─────────────────────
# -DHE_GAME_BUILD_CONFIG=<dir>/HEGameBuildConfig.h, the header the exporter
# generates (HE::writeGameBuildConfigHeader, Hpak/GameBuildConfig.h): pak name,
# startup scene, flags, ProjectSettings and the pak key become constants of this
# executable, and it reads no project.hcfg / Config/ProjectSettings.json. Unset
# (every engine build, the prebuilt runtime) = the files, as before. Only one TU
# sees the header (src/CompiledBuildConfig.cpp), so switching it rebuilds that file.
# (This replaces HE_PROJECT_SECRET, which was defined here but never read.)
set(HE_GAME_BUILD_CONFIG "" CACHE FILEPATH
	"Generated HEGameBuildConfig.h to compile into HorizonGame (empty = read project.hcfg at runtime)")
target_sources(HorizonGame PRIVATE src/CompiledBuildConfig.cpp)
if(HE_GAME_BUILD_CONFIG)
	get_filename_component(_he_bc_name "${HE_GAME_BUILD_CONFIG}" NAME)
	if(NOT EXISTS "${HE_GAME_BUILD_CONFIG}" OR NOT _he_bc_name STREQUAL "HEGameBuildConfig.h")
		message(FATAL_ERROR "HE_GAME_BUILD_CONFIG must name an existing HEGameBuildConfig.h "
		                    "(got '${HE_GAME_BUILD_CONFIG}')")
	endif()
	get_filename_component(_he_bc_dir "${HE_GAME_BUILD_CONFIG}" DIRECTORY)
	# On the one source file only: the other game TUs keep their command line.
	set_source_files_properties(src/CompiledBuildConfig.cpp PROPERTIES
		INCLUDE_DIRECTORIES "${_he_bc_dir}"
		COMPILE_DEFINITIONS HE_GAME_BUILD_CONFIG=1)
	message(STATUS "HorizonGame: project configuration compiled in from ${HE_GAME_BUILD_CONFIG}")
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