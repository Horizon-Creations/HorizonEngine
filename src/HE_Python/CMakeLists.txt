# ── HorizonPython — the CPython backend, as a runtime-loaded plugin ───────────
# This library exists so that libpython is NOT a load-time dependency of the
# engine. It used to be: HorizonScene linked Python3::Python, so every shipped
# game carried libpython (5-9 MB depending on platform) and would not even start
# without it, whether or not the project contained a single line of Python.
#
# Now HorizonScene loads this on demand through two C functions
# (Scripting/PythonPluginAbi.h) and talks to it only as an IScriptBackend*. A
# non-Python game ships neither this library nor libpython.
#
# Built only when CPython was found. When it was not, there is no plugin on
# disk, and that absence IS the "no Python support" answer — there is no second
# no-op implementation to keep in agreement with this one.
if(NOT HE_HAVE_PYTHON)
	message(STATUS "HorizonPython: CPython not found — Python scripting plugin not built")
	return()
endif()

add_library(HorizonPython SHARED
	src/PyScriptBackend.cpp
)

target_include_directories(HorizonPython PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

# HorizonScene for ScriptApi/EngineApi (the plugin calls INTO the engine; the
# engine never links back, it dlopens). No cycle: the dependency is one-way at
# build time and the other way only at runtime.
target_link_libraries(HorizonPython PRIVATE
	HorizonCore
	HorizonScene
	Python3::Python
)

target_compile_features(HorizonPython PRIVATE cxx_std_20)

# Hide everything by default so the only symbols leaving this library are the
# two the ABI names. Keeps the plugin from exporting CPython's symbols into the
# process and re-exporting engine ones, and it is the same discipline the
# Windows side gets for free (no WINDOWS_EXPORT_ALL_SYMBOLS here on purpose —
# the entry points carry __declspec(dllexport) themselves).
set_target_properties(HorizonPython PROPERTIES
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN ON
)

# Beside HorizonScene, because that is exactly where the loader looks: it asks
# the dynamic linker which directory HorizonScene itself came from (see
# thisLibraryDir() in ScriptContext.cpp). Keeping the two together is the whole
# contract, and it is the one thing to preserve when changing any layout.
set_target_properties(HorizonPython PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:HorizonScene>
	LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:HorizonScene>
)

# …and into the editor deploy alongside them. Without this a distributed editor
# would lose Python scripting SILENTLY: no missing-symbol error, just a log line
# saying no plugin was found and every .py script failing to load.
he_deploy_dll(HorizonPython)
