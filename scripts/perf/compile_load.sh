#!/bin/zsh
# Build-like background load: `n` endless loops, each recompiling one real engine
# TU (-O3, output discarded). Closer to the audit's "other instances are
# building" condition (step 3, N1) than `yes`: compilers also stress caches and
# memory bandwidth and come and go every few seconds.
#
#   scripts/perf/compile_load.sh <configured-build-dir> <n>
#
# Runs in the foreground until killed; kill the process group to stop it all.
set -u
BUILD=${1:A}; N=$2
REPO=${0:A:h:h:h}
FM=$BUILD/src/HE_Rendering/CMakeFiles/HorizonRendering.dir/flags.make
flags=()
for k in CXX_DEFINES CXX_INCLUDES CXX_FLAGS; do
  flags+=(${(z)$(sed -n "s/^$k = //p" "$FM")})
done
flags=(${(Q)flags})
TU=$REPO/src/HE_Rendering/src/RenderExtractor.cpp
for i in $(seq 1 $N); do
  ( while :; do clang++ $flags -c "$TU" -o /dev/null 2>/dev/null; done ) &
done
wait
