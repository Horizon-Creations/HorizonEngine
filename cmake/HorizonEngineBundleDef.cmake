# ─── HorizonEngine.def: what export-all gives HorizonScene, for the bundle ────
# Run as a script (cmake -P) by the HorizonEngine target on Windows, see
# HorizonEngineBundle.cmake for why the bundle does not use
# WINDOWS_EXPORT_ALL_SYMBOLS over all of its objects.
#
#   -DOBJS=<file listing HorizonScene's objects, one per line>
#   -DDEF=<the .def to write>
#
# `cmake -E __create_def` is the (undocumented) command CMake itself runs for
# WINDOWS_EXPORT_ALL_SYMBOLS, so HorizonScene's symbols come out exactly the way
# HorizonScene.dll exports them. The count goes into the build log: the import
# library holds at most 65535 members, and this file is the part of the bundle's
# exports that grows with the engine.

if(NOT OBJS OR NOT DEF)
    message(FATAL_ERROR "HorizonEngineBundleDef.cmake: pass -DOBJS=<list file> -DDEF=<output>")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E __create_def "${DEF}.tmp" "${OBJS}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "HorizonEngine.def: cmake -E __create_def failed (${_rc})")
endif()

# First line is the EXPORTS keyword, every other line one symbol.
file(STRINGS "${DEF}.tmp" _lines)
list(LENGTH _lines _count)
math(EXPR _count "${_count} - 1")
message(STATUS "HorizonEngine.def: ${_count} symbols exported from HorizonScene's objects "
               "(Core/Net/Rendering add their dllexport ones; the import library caps the total at 65535)")
if(_count GREATER 65535)
    message(FATAL_ERROR "HorizonEngine.def: ${_count} symbols, more than one import library holds")
endif()
file(RENAME "${DEF}.tmp" "${DEF}")
