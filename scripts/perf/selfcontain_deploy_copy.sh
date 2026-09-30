#!/bin/zsh
# Make a COPIED editor deploy dir load its own dylibs.
#
#   scripts/perf/selfcontain_deploy_copy.sh <copied-deploy-dir>
#
# A local build links HorizonEditor and the libHorizon*.dylib with absolute
# LC_RPATHs into the build tree (out/build/<cfg>/src/HE_Core, ...), no
# @executable_path. A `cp -R` of out/deploy/Editor therefore still loads
# whatever dylibs the build tree holds right now: two copies of different
# builds measure the same engine code. This replaces every rpath of the
# top-level Mach-O files with @loader_path and re-signs them ad hoc.
# Idempotent. Check at runtime with `lsof -p <pid> | grep libHorizonCore`.
set -eu
DIR=${1:A}
for f in "$DIR/HorizonEditor" "$DIR"/*.dylib; do
  for p in $(otool -l "$f" | awk '/LC_RPATH/{r=1} r&&/ path /{print $2; r=0}'); do
    install_name_tool -delete_rpath "$p" "$f" 2>/dev/null
  done
  install_name_tool -add_rpath @loader_path "$f" 2>/dev/null
  codesign -f -s - "$f" 2>/dev/null
done
echo "$DIR: $(otool -l "$DIR/HorizonEditor" | awk '/LC_RPATH/{r=1} r&&/ path /{print $2; r=0}' | tr '\n' ' ')"
