# ─── HorizonEngine: the engine as ONE shared library (Thema 211 step 10) ──────
# For the exported game and the SDK it is built against, and only for them. The
# editor, the tests and the tools keep linking the four libraries HorizonCore,
# HorizonNet, HorizonScene and HorizonRendering exactly as before; this target
# is added BESIDE them, not instead of them.
#
# It compiles nothing of its own: it links the object files the four libraries
# already built ($<TARGET_OBJECTS:...>), so the engine is compiled once per tree
# and the bundle costs one link. The objects are the same ones, with the same
# definitions (HE_*_BUILD_DLL, the build profile), so on macOS and Linux the
# bundle exports exactly what the four export together. On Windows the objects
# of Scene/Net/Rendering reference Core's symbols as dllimport; inside one DLL
# the linker resolves those locally (LNK4217, a warning), and Scene's symbols,
# which carry no dllexport, are exported the way HorizonScene exports them
# (WINDOWS_EXPORT_ALL_SYMBOLS).
#
# What the four link (lz4, Jolt, the backend static libraries, frameworks, ...)
# the bundle links, read from the targets below. Included from the root
# CMakeLists.txt after the four libraries and HorizonPython, before HE_Game:
# nothing adds a link library to the four after that point.
#
# Layout of an exported game (ProjectExporter, HE_Game's POST_BUILD):
#
#   <ProjectName>(.exe)
#   data/                    the pak, project.hcfg, config.json, ...
#   HorizonEngine/           HorizonEngine (.dll/.dylib/.so), SDL3, the bundled
#                            native deps, the Python plugin, Shaders/
#
# Also built here: HorizonPythonEngine, the Python plugin linked against the
# bundle instead of the four. ScriptContext loads the plugin from the directory
# of the library ScriptContext lives in, which in a game is HorizonEngine/.

include_guard(GLOBAL)

set(HE_ENGINE_BUNDLE_PARTS HorizonCore HorizonNet HorizonScene HorizonRendering)
set(HE_ENGINE_BUNDLE_DIR_NAME "HorizonEngine")
set(HE_ENGINE_BUNDLE_OUT "${CMAKE_BINARY_DIR}/engine_bundle")

# Everything in <list> that is not one of the four parts. Plain names only:
# the parts never appear inside a generator expression in these properties.
function(_he_bundle_without_parts out_var)
    set(_kept "")
    foreach(_item IN LISTS ARGN)
        if(NOT _item IN_LIST HE_ENGINE_BUNDLE_PARTS)
            list(APPEND _kept "${_item}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _kept)
    set(${out_var} "${_kept}" PARENT_SCOPE)
endfunction()

# The union of one property over the given targets.
function(_he_bundle_union out_var prop)
    set(_all "")
    foreach(_t IN LISTS ARGN)
        get_target_property(_v ${_t} ${prop})
        if(_v)
            list(APPEND _all ${_v})
        endif()
    endforeach()
    set(${out_var} "${_all}" PARENT_SCOPE)
endfunction()

set(_he_bundle_objects "")
foreach(_t IN LISTS HE_ENGINE_BUNDLE_PARTS)
    if(NOT TARGET ${_t})
        message(FATAL_ERROR "HorizonEngine bundle: ${_t} does not exist in this tree")
    endif()
    list(APPEND _he_bundle_objects "$<TARGET_OBJECTS:${_t}>")
endforeach()

add_library(HorizonEngine SHARED ${_he_bundle_objects})
add_dependencies(HorizonEngine ${HE_ENGINE_BUNDLE_PARTS})

# Link side: whatever any of the four links, PRIVATE or PUBLIC.
_he_bundle_union(_he_link LINK_LIBRARIES ${HE_ENGINE_BUNDLE_PARTS})
_he_bundle_without_parts(_he_link ${_he_link})
target_link_libraries(HorizonEngine PRIVATE ${_he_link})
# The backend static libraries link HorizonCore for its include dirs and
# definitions, so HorizonCore's own shared library comes along on the link line.
# Every symbol it would provide is defined by the objects above, so nothing is
# resolved against it, and the linker is told to drop a library nothing uses
# (MSVC records no import for an unused import library on its own). The game
# deploy checks that no part library is left among the bundle's dependencies.
if(APPLE)
    target_link_options(HorizonEngine PRIVATE "LINKER:-dead_strip_dylibs")
elseif(UNIX)
    target_link_options(HorizonEngine PRIVATE "LINKER:--as-needed")
endif()
_he_bundle_union(_he_link_opts LINK_OPTIONS ${HE_ENGINE_BUNDLE_PARTS})
if(_he_link_opts)
    list(REMOVE_DUPLICATES _he_link_opts)
    target_link_options(HorizonEngine PRIVATE ${_he_link_opts})
endif()

# Usage side: what a consumer of the four gets, as one target. The interface
# definitions stay plain (HorizonSDK.cmake writes them into the SDK package).
foreach(_prop INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
              INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS
              INTERFACE_COMPILE_FEATURES INTERFACE_LINK_LIBRARIES)
    _he_bundle_union(_v ${_prop} ${HE_ENGINE_BUNDLE_PARTS})
    _he_bundle_without_parts(_v ${_v})
    if(_v)
        set_property(TARGET HorizonEngine PROPERTY ${_prop} "${_v}")
    endif()
endforeach()

set_target_properties(HorizonEngine PROPERTIES
    LINKER_LANGUAGE CXX
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    RUNTIME_OUTPUT_DIRECTORY "${HE_ENGINE_BUNDLE_OUT}"
    LIBRARY_OUTPUT_DIRECTORY "${HE_ENGINE_BUNDLE_OUT}"
    ARCHIVE_OUTPUT_DIRECTORY "${HE_ENGINE_BUNDLE_OUT}")
if(APPLE)
    # The siblings in HorizonEngine/ (SDL3, the bundled deps) are found through
    # the loader's own directory; the game adds @executable_path/HorizonEngine.
    set_target_properties(HorizonEngine PROPERTIES
        BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(HorizonEngine PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
# No he_deploy_dll: the editor deploy keeps the four libraries and nothing else.

# ── The Python plugin, against the bundle ─────────────────────────────────────
# HorizonPython links HorizonCore + HorizonScene; beside HorizonEngine in a game
# it would either not load (no libHorizonCore there) or load a second copy of
# the engine. Same objects, same output name, beside the bundle (where HorizonScene's
# copy is not).
if(TARGET HorizonPython)
    add_library(HorizonPythonEngine SHARED "$<TARGET_OBJECTS:HorizonPython>")
    add_dependencies(HorizonPythonEngine HorizonPython)
    get_target_property(_he_py_link HorizonPython LINK_LIBRARIES)
    list(REMOVE_ITEM _he_py_link ${HE_ENGINE_BUNDLE_PARTS})
    target_link_libraries(HorizonPythonEngine PRIVATE HorizonEngine ${_he_py_link})
    set_target_properties(HorizonPythonEngine PROPERTIES
        OUTPUT_NAME HorizonPython
        LINKER_LANGUAGE CXX
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        RUNTIME_OUTPUT_DIRECTORY "${HE_ENGINE_BUNDLE_OUT}"
        LIBRARY_OUTPUT_DIRECTORY "${HE_ENGINE_BUNDLE_OUT}"
        ARCHIVE_OUTPUT_DIRECTORY "${HE_ENGINE_BUNDLE_OUT}")
    if(APPLE)
        # libpython stays beside the executable (its stdlib bootstrap is
        # executable-relative), which the game's @executable_path rpath reaches.
        set_target_properties(HorizonPythonEngine PROPERTIES
            BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
    elseif(UNIX)
        set_target_properties(HorizonPythonEngine PROPERTIES INSTALL_RPATH "$ORIGIN;$ORIGIN/..")
    endif()
endif()

message(STATUS "HorizonEngine: bundle target HorizonEngine (${HE_ENGINE_BUNDLE_PARTS}) for the game and the SDK")
