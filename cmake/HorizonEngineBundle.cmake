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
# (export-all over Scene's objects, through a generated .def; see below).
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

# Does <tgt> reach one of the four parts through LINK_LIBRARIES?
function(_he_bundle_reaches_part out_var tgt)
    set(${out_var} FALSE PARENT_SCOPE)
    get_property(_seen GLOBAL PROPERTY _HE_BUNDLE_WALKED)
    if(tgt IN_LIST _seen)
        get_property(_r GLOBAL PROPERTY _HE_BUNDLE_REACH_${tgt})
        set(${out_var} ${_r} PARENT_SCOPE)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY _HE_BUNDLE_WALKED ${tgt})
    set_property(GLOBAL PROPERTY _HE_BUNDLE_REACH_${tgt} FALSE)
    get_target_property(_libs ${tgt} LINK_LIBRARIES)
    if(NOT _libs)
        return()
    endif()
    foreach(_l IN LISTS _libs)
        if(_l IN_LIST HE_ENGINE_BUNDLE_PARTS)
            set(_hit TRUE)
        elseif(TARGET ${_l})
            _he_bundle_reaches_part(_hit ${_l})
        else()
            set(_hit FALSE)
        endif()
        if(_hit)
            set_property(GLOBAL PROPERTY _HE_BUNDLE_REACH_${tgt} TRUE)
            set(${out_var} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# What goes into the bundle: the four parts, and every static library of this
# tree they link that itself links one of them (the renderer backends,
# he_materialshader, he_shadercompiler all link HorizonCore for its headers).
# Those come in as OBJECTS too, so no part library is ever on the bundle's link
# line. Linked as libraries they would bring HorizonCore along, and on Windows
# Scene's __imp_ references to Core would then resolve against HorizonCore.lib:
# HorizonEngine.dll would import HorizonCore.dll, the game would run two copies
# of the engine's state, and TARGET_RUNTIME_DLLS would ship the parts beside it.
set(_he_bundle_objects "")
set(_he_bundle_absorbed "")
set(_he_link "")
set(_he_queue ${HE_ENGINE_BUNDLE_PARTS})
while(_he_queue)
    list(POP_FRONT _he_queue _t)
    if(NOT TARGET ${_t})
        message(FATAL_ERROR "HorizonEngine bundle: ${_t} does not exist in this tree")
    endif()
    list(APPEND _he_bundle_objects "$<TARGET_OBJECTS:${_t}>")
    list(APPEND _he_bundle_absorbed ${_t})
    get_target_property(_libs ${_t} LINK_LIBRARIES)
    if(NOT _libs)
        continue()
    endif()
    foreach(_l IN LISTS _libs)
        if(_l IN_LIST HE_ENGINE_BUNDLE_PARTS OR _l IN_LIST _he_bundle_absorbed
           OR _l IN_LIST _he_queue)
            continue()
        endif()
        set(_absorb FALSE)
        if(TARGET ${_l})
            get_target_property(_type ${_l} TYPE)
            get_target_property(_imported ${_l} IMPORTED)
            get_target_property(_aliased ${_l} ALIASED_TARGET)
            if(_type STREQUAL "STATIC_LIBRARY" AND NOT _imported AND NOT _aliased)
                _he_bundle_reaches_part(_absorb ${_l})
            endif()
        endif()
        if(_absorb)
            list(APPEND _he_queue ${_l})
        else()
            list(APPEND _he_link ${_l})
        endif()
    endforeach()
endwhile()
list(REMOVE_ITEM _he_link ${_he_bundle_absorbed})
list(REMOVE_DUPLICATES _he_link)
list(REMOVE_ITEM _he_bundle_absorbed ${HE_ENGINE_BUNDLE_PARTS})

# Imported targets are directory-scoped: OpenGL::GL (HE_Rendering), Vulkan::Vulkan,
# CURL::libcurl (HE_Net) were found in a subdirectory and are not visible here.
# Found again at this scope; a package that still has no target is an error,
# not a silently dropped library.
foreach(_l IN LISTS _he_link)
    if(_l MATCHES "^([A-Za-z0-9_]+)::" AND NOT TARGET ${_l})
        find_package(${CMAKE_MATCH_1} QUIET)
        if(NOT TARGET ${_l})
            message(FATAL_ERROR "HorizonEngine bundle: ${_l} (linked by an engine library) "
                                "is not a target at the root scope")
        endif()
    endif()
endforeach()

add_library(HorizonEngine SHARED ${_he_bundle_objects})
add_dependencies(HorizonEngine ${HE_ENGINE_BUNDLE_PARTS} ${_he_bundle_absorbed})

# Link side: whatever the parts and the absorbed static libraries link, PRIVATE
# or PUBLIC, but none of them.
target_link_libraries(HorizonEngine PRIVATE ${_he_link})
# Belt and braces on top of the above: drop a shared library nothing resolves
# against, should one reach the link line some other way.
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
    RUNTIME_OUTPUT_DIRECTORY "${HE_ENGINE_BUNDLE_OUT}"
    LIBRARY_OUTPUT_DIRECTORY "${HE_ENGINE_BUNDLE_OUT}"
    ARCHIVE_OUTPUT_DIRECTORY "${HE_ENGINE_BUNDLE_OUT}")
if(WIN32)
    # Export-all over Scene's objects only, NOT WINDOWS_EXPORT_ALL_SYMBOLS on the
    # bundle. Over every object (the four plus the absorbed backends and shader
    # compilers) it put each external symbol of the engine, inline and template
    # instantiations included, into HorizonEngine.lib, one import-library member
    # per symbol, and the import library holds 65535: LNK1189 (CI 39168157588).
    # Of the four only HorizonScene relies on export-all; Core, Net and Rendering
    # export through HE_*_BUILD_DLL dllexport, which their objects carry into the
    # bundle unchanged. So the bundle exports the union the four export one by
    # one, each of which links under the limit. Linking the four as static
    # archives instead does not help: export-all never walks archive members,
    # and the linker would drop every Scene object nothing in the link uses.
    # Must stay OFF: with it ON CMake folds a .def source into its own all-
    # objects .def, which is the failure again.
    set_target_properties(HorizonEngine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    set(_he_def_dir "${CMAKE_BINARY_DIR}/CMakeFiles/HorizonEngine.def.dir")
    set(_he_def_objs "${_he_def_dir}/scene_objects.txt")
    set(_he_def "${_he_def_dir}/HorizonEngine.def")
    file(GENERATE OUTPUT "${_he_def_objs}"
         CONTENT "$<JOIN:$<TARGET_OBJECTS:HorizonScene>,\n>\n")
    add_custom_command(OUTPUT "${_he_def}"
        COMMAND "${CMAKE_COMMAND}" "-DOBJS=${_he_def_objs}" "-DDEF=${_he_def}"
                -P "${CMAKE_SOURCE_DIR}/cmake/HorizonEngineBundleDef.cmake"
        DEPENDS "${_he_def_objs}" "$<TARGET_OBJECTS:HorizonScene>"
                "${CMAKE_SOURCE_DIR}/cmake/HorizonEngineBundleDef.cmake"
        COMMENT "Exports of HorizonEngine.dll (HorizonScene's objects)"
        VERBATIM)
    target_sources(HorizonEngine PRIVATE "${_he_def}")
elseif(APPLE)
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

message(STATUS "HorizonEngine: bundle target HorizonEngine (${HE_ENGINE_BUNDLE_PARTS}; as objects too: ${_he_bundle_absorbed}) for the game and the SDK")
