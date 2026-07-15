# he_bundle_python(<target> <dest>)
#
# Adds a POST_BUILD step to <target> that bundles the CPython runtime into <dest>:
# python3XY.dll (+ python3.dll), a curated pure-Python stdlib zip (python3XY.zip via
# cmake/zip_stdlib.py), and a python3XY._pth so embedded Python resolves the bundled
# stdlib in isolated path mode. Needed because anything linking Python3::Python (e.g.
# HorizonScene.dll) imports python3XY.dll in its LOAD-TIME import table — so both the
# shipped game AND the shipped editor need it next to the executable, or they fail on a
# clean PC ("python314.dll fehlt"). No-op unless WIN32 + HE_HAVE_PYTHON and the DLL/stdlib
# are located. Requires the Python3 find_package (Interpreter + Development.Embed).
function(he_bundle_python target dest)
	if(NOT HE_HAVE_PYTHON)
		return()
	endif()

	# ── Linux: bundle libpython next to the executable ──────────────────────────
	# The editor/game link Python3::Python (via HorizonScene), so libpythonMAJ.MIN.so
	# is a LOAD-TIME dependency — a downloaded package must carry it or it won't even
	# launch on a machine without a system Python. Copy the versioned SONAME file into
	# <dest>; the $ORIGIN rpath (see root CMakeLists) finds it. (The pure-Python stdlib
	# is not bundled here: embedded Py_Initialize() resolves it from the system python3
	# install — running Python scripts on Linux needs a matching system python3.)
	if(UNIX AND NOT APPLE)
		set(_py_maj "${Python3_VERSION_MAJOR}")
		set(_py_min "${Python3_VERSION_MINOR}")
		set(_py_soname "libpython${_py_maj}.${_py_min}.so.1.0")
		find_file(HE_PYTHON_SO
			NAMES "${_py_soname}" "libpython${_py_maj}.${_py_min}.so"
			HINTS ${Python3_RUNTIME_LIBRARY_DIRS} ${Python3_LIBRARY_DIRS}
			      /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu
			      /usr/lib /usr/local/lib)
		if(HE_PYTHON_SO)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E make_directory "${dest}"
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"${HE_PYTHON_SO}" "${dest}/${_py_soname}"
				COMMENT "Bundling CPython runtime (${_py_soname}) into ${dest}"
				VERBATIM)
		else()
			message(WARNING "he_bundle_python(${target}): libpython not located on Linux — "
				"the shipped editor will rely on a system Python install "
				"(searched for ${_py_soname})")
		endif()
		return()
	endif()

	if(NOT WIN32)
		return()
	endif()
	set(_py_tag "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}") # e.g. python314
	get_filename_component(_py_exedir "${Python3_EXECUTABLE}" DIRECTORY)
	find_file(HE_PYTHON_DLL "${_py_tag}.dll"
		HINTS ${Python3_RUNTIME_LIBRARY_DIRS} "${_py_exedir}" NO_DEFAULT_PATH)
	find_file(HE_PYTHON_DLL_STABLE "python3.dll"
		HINTS ${Python3_RUNTIME_LIBRARY_DIRS} "${_py_exedir}" NO_DEFAULT_PATH)
	if(HE_PYTHON_DLL AND Python3_Interpreter_FOUND AND Python3_STDLIB)
		set(_pth "${CMAKE_BINARY_DIR}/${_py_tag}._pth")
		file(WRITE "${_pth}" "${_py_tag}.zip\n.\n")
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory "${dest}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HE_PYTHON_DLL}" "${dest}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_pth}" "${dest}"
			COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/cmake/zip_stdlib.py"
				"${Python3_STDLIB}" "${dest}/${_py_tag}.zip"
			COMMENT "Bundling CPython runtime (${_py_tag}) into ${dest}"
			VERBATIM)
		if(HE_PYTHON_DLL_STABLE)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HE_PYTHON_DLL_STABLE}" "${dest}"
				VERBATIM)
		endif()
	else()
		message(WARNING "he_bundle_python(${target}): Python enabled but the interpreter DLL/stdlib "
			"was not located — the shipped build will be missing the Python runtime "
			"(HE_PYTHON_DLL='${HE_PYTHON_DLL}', Python3_STDLIB='${Python3_STDLIB}')")
	endif()
endfunction()

# he_bundle_python_stdlib(<target> <dest> <exe_name>)
#
# GAME-ONLY, macOS + Linux: bundle the pure-Python stdlib so a Python-language export
# runs embedded Python on a clean machine. Produces two files in <dest>:
#   • pythonXY.zip  — curated pure-Python stdlib, ZIP_STORED (uncompressed). MUST be
#     stored: a deflate zip can't be read at interpreter bootstrap there (zlib is an
#     unbundled lib-dynload extension → "Failed to import encodings module").
#   • lib-dynload/  — the C-extension modules (struct/datetime/random/socket/… need
#     these; they can't live in a zip). copy_pydynload.py copies all except the few
#     that pull in extra native libs a game doesn't need (tkinter/curses/ssl/…).
#   • <exe_name>._pth — "pythonXY.zip\n.\nlib-dynload\n". On POSIX getpath looks for a
#     <executable-name>._pth next to the exe (NOT pythonXY._pth — that's the Windows
#     DLL landmark) and, when present, runs isolated with exactly those paths. Plain
#     Py_Initialize() then boots from the bundle — no runtime code change.
# The libpython dylib/.so itself is bundled elsewhere (macOS: bundle_native_deps BFS;
# Linux: he_bundle_python). Windows ships its own zip + pythonXY._pth via he_bundle_python.
# Call ONLY for the game — never the editor (a ._pth would force the editor onto the
# curated stdlib instead of the full system Python). The exporter ships these files
# only when the project's language is Python (ExportSettings::bundlePythonStdlib).
function(he_bundle_python_stdlib target dest exe_name)
	if(NOT HE_HAVE_PYTHON)
		return()
	endif()
	if(NOT UNIX)               # Windows handled by he_bundle_python (frozen zlib → deflate + DLL landmark)
		return()
	endif()
	if(NOT (Python3_Interpreter_FOUND AND Python3_STDLIB))
		message(WARNING "he_bundle_python_stdlib(${target}): no Python interpreter/stdlib located — "
			"a Python-language game export will be missing its stdlib")
		return()
	endif()
	set(_py_tag "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}")
	set(_pth "${CMAKE_BINARY_DIR}/${exe_name}._pth")
	file(WRITE "${_pth}" "${_py_tag}.zip\n.\nlib-dynload\n")
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${dest}"
		COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/cmake/zip_stdlib.py"
			"${Python3_STDLIB}" "${dest}/${_py_tag}.zip" store
		COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/cmake/copy_pydynload.py"
			"${Python3_STDLIB}/lib-dynload" "${dest}/lib-dynload"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_pth}" "${dest}/${exe_name}._pth"
		COMMENT "Bundling curated Python stdlib (${_py_tag}.zip + lib-dynload) + ${exe_name}._pth into ${dest}"
		VERBATIM)
endfunction()
