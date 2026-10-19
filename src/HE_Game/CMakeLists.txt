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

# The engine as ONE library (cmake/HorizonEngineBundle.cmake): the game and
# the SDK use HorizonEngine, the editor, tests and tools the four libraries it
# is made of.
target_link_libraries(HorizonGame PRIVATE
	HorizonEngine
	SDL3::SDL3
)

# Windows has no rpath and does not look into HorizonEngine\ on its own: the
# engine and SDL3 are delay-loaded, and EngineDllDir.cpp's delay-load hook loads
# them from there on first use (their own imports, the bundled deps, then
# resolve beside them). CRT and python3XY.dll stay beside the executable, where
# the loader looks anyway.
if(MSVC)
	target_sources(HorizonGame PRIVATE src/EngineDllDir.cpp)
	target_link_options(HorizonGame PRIVATE
		"/DELAYLOAD:$<TARGET_FILE_NAME:HorizonEngine>"
		"/DELAYLOAD:$<TARGET_FILE_NAME:SDL3::SDL3>")
	target_link_libraries(HorizonGame PRIVATE delayimp)
endif()

# @executable_path/HorizonEngine: the engine, SDL3 and the bundled deps.
# @executable_path: libpython, which stays beside the executable (its stdlib
# bootstrap, HorizonGame._pth, is executable-relative). Linux: the same through
# $ORIGIN, prepended to the root's CMAKE_BUILD_RPATH.
if(APPLE)
    set_target_properties(HorizonGame PROPERTIES
        BUILD_RPATH "@executable_path/${HE_ENGINE_BUNDLE_DIR_NAME};@executable_path"
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
elseif(UNIX)
    set_target_properties(HorizonGame PROPERTIES
        BUILD_RPATH "$ORIGIN/${HE_ENGINE_BUNDLE_DIR_NAME};$ORIGIN"
    )
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
	# OBJECT_DEPENDS because Make's dependency scan does not follow a per-source
	# include dir: without it a rewritten header (every export writes one) left
	# the old configuration compiled in.
	set_source_files_properties(src/CompiledBuildConfig.cpp PROPERTIES
		INCLUDE_DIRECTORIES "${_he_bc_dir}"
		COMPILE_DEFINITIONS HE_GAME_BUILD_CONFIG=1
		OBJECT_DEPENDS "${HE_GAME_BUILD_CONFIG}")
	message(STATUS "HorizonGame: project configuration compiled in from ${HE_GAME_BUILD_CONFIG}")
endif()

# ── Post-build: deploy HorizonGame + the HorizonEngine/ folder ───────────────
# Uses DEPLOY_GAME from the root CMakeLists.txt. The exporter ships this folder
# as it is, the executable renamed after the project:
#
#   HorizonGame(.exe)  + the Python runtime (libpython / python3XY.dll, stdlib
#                        zip, _pth, lib-dynload) and the MSVC runtime
#   HorizonEngine/     the engine library, SDL3, the Python plugin, the bundled
#                      native deps, Shaders/
#
# The engine libraries of the old flat layout are removed first, so an export
# from an updated deploy cannot ship them beside the new ones.
set(_he_game_engine_dir "${DEPLOY_GAME}/${HE_ENGINE_BUNDLE_DIR_NAME}")
add_custom_command(TARGET HorizonGame POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "${_he_game_engine_dir}"
	COMMAND ${CMAKE_COMMAND} -E rm -f
		"${DEPLOY_GAME}/$<TARGET_FILE_NAME:HorizonCore>"
		"${DEPLOY_GAME}/$<TARGET_FILE_NAME:HorizonScene>"
		"${DEPLOY_GAME}/$<TARGET_FILE_NAME:HorizonRendering>"
		"${DEPLOY_GAME}/$<TARGET_FILE_NAME:HorizonNet>"
		"${DEPLOY_GAME}/$<TARGET_FILE_NAME:SDL3::SDL3>"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:HorizonGame>
		"${DEPLOY_GAME}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:HorizonEngine>
		$<TARGET_FILE:SDL3::SDL3>
		"${_he_game_engine_dir}"
	COMMENT "Deploying HorizonGame to ${DEPLOY_GAME} (engine in ${HE_ENGINE_BUNDLE_DIR_NAME}/)"
	VERBATIM
)
if(WIN32)
	# Whatever else HorizonEngine.dll imports from an imported target (a vcpkg
	# lz4/zstd/OpenSSL), beside it.
	add_custom_command(TARGET HorizonGame POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_RUNTIME_DLLS:HorizonEngine> "${_he_game_engine_dir}"
		COMMAND_EXPAND_LISTS
		VERBATIM)
endif()

# The Python plugin, when the engine has one: the variant linked against
# HorizonEngine, beside it (ScriptContext looks in the directory of the library
# it lives in). It goes into the runtime TEMPLATE unconditionally — the template
# must be able to serve any project — while the exporter decides per project
# whether it reaches the finished game (see ProjectExporter).
if(HE_HAVE_PYTHON)
	add_custom_command(TARGET HorizonGame POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E rm -f "${DEPLOY_GAME}/$<TARGET_FILE_NAME:HorizonPython>"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:HorizonPythonEngine>
			"${_he_game_engine_dir}"
		COMMENT "Deploying the Python plugin to ${_he_game_engine_dir}"
		VERBATIM)
	add_dependencies(HorizonGame HorizonPythonEngine)
endif()

# Runtime-loaded shader binaries (Vulkan .spv + D3D12 DXR .cso) live in
# ${CMAKE_BINARY_DIR}/Shaders; the backends load them from Shaders/ beside the
# engine library's folder name (HorizonEngine/Shaders, falling back to
# <exe dir>/Shaders of the old layout and of the editor deploy). Without this a
# packaged game loses Vulkan entirely and D3D12 GI silently drops to the
# software kernels.
if(HE_VULKAN_ENABLED OR HE_D3D12_GI_SHADERS)
	add_custom_command(TARGET HorizonGame POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E rm -rf "${DEPLOY_GAME}/Shaders"
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${CMAKE_BINARY_DIR}/Shaders"
			"${_he_game_engine_dir}/Shaders"
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
# bundles those deps into HorizonEngine/ beside the engine library and (macOS)
# rewrites the links to @rpath + ad-hoc re-signs; what already sits beside the
# executable (libpython) is not copied a second time. No-op on Windows (bash-less — Python/CRT handled above).
# MUST run last, after the deploy + he_bundle_python have populated DEPLOY_GAME.
if(UNIX)
    add_custom_command(TARGET HorizonGame POST_BUILD
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/bundle_native_deps.sh" "${_he_game_engine_dir}" "${DEPLOY_GAME}"
        COMMENT "Bundling native deps into ${_he_game_engine_dir} (self-contained game runtime)"
        VERBATIM)
endif()