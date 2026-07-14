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
# import table — a shipped game therefore needs python3XY.dll next to the executable
# even if it never runs a Python script, plus the standard library once it does
# (Py_Initialize imports `encodings`/`io`/… at startup). On the dev box these resolve
# via PATH/registry, so the game runs there but fails on a clean PC ("python314.dll
# fehlt"). We copy the interpreter DLL(s), zip the stdlib to python3XY.zip, and write a
# python3XY._pth next to the DLL so embedded Python resolves the bundled stdlib in
# isolated path mode (no dependency on an installed Python). The Editor's exporter copies
# these flat files next to the exe on a Windows export (see ProjectExporter).
if(WIN32 AND HE_HAVE_PYTHON)
	set(_py_tag "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}") # e.g. python314
	get_filename_component(_py_exedir "${Python3_EXECUTABLE}" DIRECTORY)
	find_file(HE_PYTHON_DLL "${_py_tag}.dll"
		HINTS ${Python3_RUNTIME_LIBRARY_DIRS} "${_py_exedir}" NO_DEFAULT_PATH)
	find_file(HE_PYTHON_DLL_STABLE "python3.dll"
		HINTS ${Python3_RUNTIME_LIBRARY_DIRS} "${_py_exedir}" NO_DEFAULT_PATH)
	if(HE_PYTHON_DLL AND Python3_Interpreter_FOUND AND Python3_STDLIB)
		# python3XY._pth: bundled stdlib zip first, then the exe dir. Its mere presence
		# next to python3XY.dll switches embedded Python to isolated path mode.
		file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${_py_tag}._pth" "${_py_tag}.zip\n.\n")
		add_custom_command(TARGET HorizonGame POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_GAME}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HE_PYTHON_DLL}" "${DEPLOY_GAME}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${CMAKE_CURRENT_BINARY_DIR}/${_py_tag}._pth" "${DEPLOY_GAME}"
			# Zip the pure-Python stdlib to <tag>.zip with modules at the archive root,
			# curated (no __pycache__/tests/tkinter/...) so the bundle is ~3 MB not ~200.
			COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/cmake/zip_stdlib.py"
				"${Python3_STDLIB}" "${DEPLOY_GAME}/${_py_tag}.zip"
			COMMENT "Bundling CPython runtime (${_py_tag}.dll + ${_py_tag}.zip stdlib) to ${DEPLOY_GAME}"
			VERBATIM)
		if(HE_PYTHON_DLL_STABLE)
			add_custom_command(TARGET HorizonGame POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HE_PYTHON_DLL_STABLE}" "${DEPLOY_GAME}"
				VERBATIM)
		endif()
	else()
		message(WARNING "HorizonEngine: Python scripting enabled but the interpreter DLL/stdlib "
			"could not be located — shipped Windows games will be missing the Python runtime "
			"(HE_PYTHON_DLL='${HE_PYTHON_DLL}', Python3_STDLIB='${Python3_STDLIB}')")
	endif()
endif()