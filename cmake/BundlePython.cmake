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
	if(NOT (WIN32 AND HE_HAVE_PYTHON))
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
