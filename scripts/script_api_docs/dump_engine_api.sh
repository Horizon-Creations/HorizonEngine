#!/bin/sh
# Compile dump_engine_api.cpp against an existing build tree and write the
# registry as JSON.
#
#   scripts/script_api_docs/dump_engine_api.sh [BUILD_DIR] [OUT_JSON]
#
# BUILD_DIR defaults to <repo>/build. From a worktree pass the main checkout's
# build dir. The headers come from THIS checkout and the libraries from BUILD_DIR,
# so the build must be of the same EngineApi.h (a changed ApiFn layout would read
# garbage) and not older than the last EngineApi.cpp change you want to see.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
B=${1:-$REPO/build}
OUT=${2:-$HERE/registry.json}
EXE=${TMPDIR:-/tmp}/he_dump_engine_api

LIB=$B/src/HE_Scene/libHorizonScene.dylib
[ -f "$LIB" ] || { echo "no $LIB - build HorizonScene first" >&2; exit 1; }

clang++ -std=gnu++20 -DHE_CORE_DLL -DHE_SCENE_DLL -DHE_NET_DLL -DHE_RENDERING_BUILD_DLL \
    -I"$B/generated" -I"$REPO/src/HE_Editor" \
    -I"$REPO/src/HE_Core/include" -I"$REPO/src/HE_Core/vendor" \
    -I"$REPO/src/HE_Net/include" -I"$REPO/src/HE_Scene/include" -I"$REPO/src/HE_Scene/vendor" \
    -I"$REPO/src/HE_Rendering/include" \
    -I"$B/_deps/glm-src" -I"$B/_deps/nlohmann_json-src/single_include" \
    -I"$B/_deps/sdl3-src/include" -I"$B/_deps/sdl3-build/include-revision" \
    "$HERE/dump_engine_api.cpp" "$REPO/src/HE_Editor/HcNodeDocs.cpp" \
    -L"$B/src/HE_Scene" -lHorizonScene -L"$B/src/HE_Core" -lHorizonCore \
    -Wl,-rpath,"$B/src/HE_Scene" -Wl,-rpath,"$B/src/HE_Core" -Wl,-rpath,"$B/src/HE_Net" \
    -Wl,-rpath,"$B/src/HE_Rendering" -Wl,-rpath,"$B/src/HE_Editor" \
    -o "$EXE"

"$EXE" > "$OUT"
python3 -c "import json,sys; print(len(json.load(open(sys.argv[1]))), 'rows ->', sys.argv[1])" "$OUT"
