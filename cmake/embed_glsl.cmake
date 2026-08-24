# embed_glsl.cmake — macht aus einer .glsl-Datei einen C++-Header mit genau dem
# Stringliteral, das vorher von Hand im Quelltext stand.
#
# Warum ueberhaupt: der analytische Himmelskern lag dreifach im Baum und ist
# auseinandergelaufen (siehe shaders/sky_core.glsl). Vulkan kann die Datei per
# #include ziehen, OpenGL nicht — dort ist der Shader ein C++-Stringliteral, das
# zur Laufzeit an den //#SKYFUNC#-Marker gespleisst wird. Dieser Schritt baut
# genau dieses Literal aus derselben Datei, statt es ein zweites Mal zu pflegen.
#
# Aufruf (Skriptmodus):
#   cmake -DIN=<datei.glsl> -DOUT=<header.h> -DVAR=<C++-Name> -P embed_glsl.cmake
#
# Das erzeugte Literal beginnt mit einem Zeilenumbruch direkt nach R"GLSL( und
# endet mit dem Zeilenumbruch der letzten Quellzeile — dieselbe Form wie das
# von Hand geschriebene Literal, damit der an GL uebergebene Text sich nicht
# durch den Umbau um ein einziges Zeichen verschiebt.
#
# Der Trennbezeichner ist GLSLSRC statt GLSL: der Dateiinhalt enthaelt selbst
# die Sequenz )GLSL" nicht, aber ein kuenftiger Kern koennte sie enthalten, und
# ein Raw-String, der mitten im Shader endet, ist ein Compilerfehler an einer
# voellig anderen Stelle.

if(NOT DEFINED IN OR NOT DEFINED OUT OR NOT DEFINED VAR)
    message(FATAL_ERROR "embed_glsl: IN, OUT und VAR muessen gesetzt sein")
endif()

file(READ "${IN}" _src)

string(FIND "${_src}" ")GLSLSRC\"" _clash)
if(NOT _clash EQUAL -1)
    message(FATAL_ERROR
        "embed_glsl: ${IN} enthaelt die Trennsequenz )GLSLSRC\" — Rohliteral waehle neu")
endif()

get_filename_component(_name "${IN}" NAME)
set(_out "// AUTOMATISCH ERZEUGT aus shaders/${_name} — nicht von Hand aendern.\n")
string(APPEND _out "// Quelle bearbeiten, nicht diesen Header; erzeugt von cmake/embed_glsl.cmake.\n")
string(APPEND _out "#pragma once\n\n")
string(APPEND _out "static const char* const ${VAR} = R\"GLSLSRC(\n")
string(APPEND _out "${_src}")
string(APPEND _out ")GLSLSRC\";\n")

# Nur schreiben, wenn sich etwas geaendert hat — sonst stossen wir bei jedem
# Build eine Neuuebersetzung von OpenGLRenderer.cpp an (eine 590-KB-TU).
set(_old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _out)
    file(WRITE "${OUT}" "${_out}")
endif()
