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
list(LENGTH _lines _all)
math(EXPR _all "${_all} - 1")

# Export-all lists every external symbol of the objects, and most of them are instantiations of standard-library and entt
# templates (a vector's members for every element type the scene code uses). Nobody imports those: a .def export puts no
# dllimport on anything, so every module that uses such a template instantiates its own copy (they are inline). They only
# fill the import library, which holds 65535 members, and they are what grows with every new file (LNK1189 on
# 2026-11-22: 62927 from here plus what Core, Net and Rendering dllexport). Taken out by their mangled names:
#   members and special members of a class template of that name      ?name@?$vector@ ...   ??0?$vector@ ...   ??_G?$...
#   the library's internals, which begin with an underscore and a capital   ??$_Emplace_one_at_back@ ...   ?_Tidy@?$ ...
# The engine has no template of these names and nothing that begins with an underscore and a capital (reserved names).
set(_std_templates
    "vector|basic_string|basic_string_view|unordered_map|unordered_set|unordered_multimap|map|multimap|set|multiset|list|deque|array"
    "|function|shared_ptr|weak_ptr|unique_ptr|optional|variant|pair|tuple|allocator|initializer_list|atomic|chrono"
    "|basic_ostream|basic_istream|basic_ostringstream|basic_istringstream|basic_stringstream|basic_stringbuf|basic_ifstream|basic_ofstream|basic_filebuf|basic_ios|basic_streambuf"
    "|basic_storage|basic_sparse_set|basic_registry|basic_view|basic_group|basic_entt_traits|basic_any|basic_common_type|sigh|sigh_mixin|delegate|compressed_pair|dense_map|dense_set")
string(REPLACE ";" "" _std_templates "${_std_templates}")
set(_kept "${_lines}")
list(FILTER _kept EXCLUDE REGEX "^[ \t]*\\?(\\?_?[0-9A-Z]|[A-Za-z_][A-Za-z0-9_]*@)\\?\\$(${_std_templates})@")
list(FILTER _kept EXCLUDE REGEX "^[ \t]*\\?\\?\\$_[A-Z]")
list(FILTER _kept EXCLUDE REGEX "^[ \t]*\\?_[A-Z][A-Za-z0-9_]*@\\?\\$")
list(LENGTH _kept _count)
math(EXPR _count "${_count} - 1")
math(EXPR _dropped "${_all} - ${_count}")
message(STATUS "HorizonEngine.def: ${_count} symbols exported from HorizonScene's objects, ${_dropped} standard-library and entt "
               "template instantiations left out of ${_all} "
               "(Core/Net/Rendering add their dllexport ones; the import library caps the total at 65535)")
if(_count GREATER 60000)
    message(WARNING "HorizonEngine.def: ${_count} symbols: close to what one import library holds with the other modules' exports")
endif()
if(_count GREATER 65535)
    message(FATAL_ERROR "HorizonEngine.def: ${_count} symbols, more than one import library holds")
endif()
list(JOIN _kept "\n" _text)
file(WRITE "${DEF}" "${_text}\n")
file(REMOVE "${DEF}.tmp")
