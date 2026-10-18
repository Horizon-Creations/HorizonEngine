#!/bin/zsh
# Build (and optionally run) physics_load_bench.cpp against the engine's own Jolt (v5.5.0, the same
# defines the engine's Release build uses: JPH_DEBUG_RENDERER JPH_OBJECT_STREAM JPH_PROFILE_ENABLED).
#   scripts/perf/physics_load_bench.sh build [outdir]
# JOLT_BUILD  = a CMake build tree that has _deps/joltphysics-build/libJolt.a (default: the main checkout's out/build/macos-release)
set -eu
HERE=${0:A:h}
JOLT_BUILD=${JOLT_BUILD:-/Users/connorjansen/VSCode/HorizonEngine/out/build/macos-release}
OUT=${2:-/tmp/he178}
case "${1:-build}" in
  build)
    mkdir -p $OUT
    clang++ -O2 -std=c++17 -arch arm64 -pthread -fno-rtti -fno-exceptions -ffp-model=precise -faligned-allocation \
      -DJPH_DEBUG_RENDERER -DJPH_OBJECT_STREAM -DJPH_PROFILE_ENABLED -DNDEBUG \
      -I$JOLT_BUILD/_deps/joltphysics-src/Build/.. \
      $HERE/physics_load_bench.cpp $JOLT_BUILD/_deps/joltphysics-build/libJolt.a -o $OUT/physics_load_bench
    echo "built $OUT/physics_load_bench" ;;
  *) echo "usage: $0 build [outdir]"; exit 2 ;;
esac
