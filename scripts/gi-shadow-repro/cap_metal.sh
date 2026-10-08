#!/bin/zsh
# One headless GI capture on Metal (Thema 131, Schritt 7) -- the macOS twin of cap.ps1.
# usage: scripts/gi-shadow-repro/cap_metal.sh NAME [ENV=VAL ...]
#   e.g. cap_metal.sh hw_gi1 HE_DUMP_GI=1 HE_DUMP_FRAMES=60
#        cap_metal.sh sw_gi1 HE_DUMP_GI=1 HE_GI_FORCE_SW=1
#        GI_CAP_CONFIG=scripts/gi-shadow-repro/config_r6.json cap_metal.sh r6_f60 HE_DUMP_GI=1 HE_DUMP_FRAMES=60
#        cap_metal.sh refl HE_DUMP_SHADOWINSTTEST= HE_DUMP_GIREFLTEST=1 HE_DUMP_GI=1 HE_DUMP_GIREFL=1 \
#                     HE_DUMP_TOD=0.5 HE_DUMP_PITCH=-8 HE_DUMP_CAMX=0 HE_DUMP_CAMY=2.5 HE_DUMP_CAMZ=2
# Runs THIS tree's out/deploy/Editor (build macos-release first), private empty
# HE_CONFIG_DIR per capture, log via `script -q` (line-buffered; the Metal dump
# teardown can segfault after the BMP). Output: $GI_CAP_OUT (default /tmp/gi-cap)/NAME.{bmp,log}.
# The Metal renderer logs no success line for its GI pipelines, only failures:
# grep the log for "failed" and read the "Session summary ... error(s)" line.
# See docs/gi-shadow-edge-noise-analysis-2026-10-02.md §7.6.
NAME=$1; shift
ROOT=${0:A:h:h:h}
ED=$ROOT/out/deploy/Editor
OUT=${GI_CAP_OUT:-/tmp/gi-cap}
mkdir -p $OUT
rm -rf $OUT/cfg-$NAME; mkdir -p $OUT/cfg-$NAME
# GI_CAP_CONFIG=<config.json template> (e.g. config_r6.json for GILightRadius 6°), like cap.ps1 -Config.
[ -n "$GI_CAP_CONFIG" ] && cp "$GI_CAP_CONFIG" $OUT/cfg-$NAME/config.json
rm -f $OUT/$NAME.bmp $OUT/$NAME.log
cd $ED || exit 1
env -i HOME=$HOME PATH=$PATH USER=$USER TMPDIR=$TMPDIR \
  HE_CONFIG_DIR=$OUT/cfg-$NAME HE_COLLAB_OFFLINE=1 HE_NET_LOOPBACK_ONLY=1 \
  HE_SKY_TIME=30 HE_DUMP_PATH=$OUT/$NAME.bmp HE_DUMP_QUIT=1 HE_DUMP_RHI=Metal \
  HE_DUMP_SKYTEST=1 HE_DUMP_SHADOWINSTTEST=1 HE_DUMP_TOD=0.35 HE_DUMP_PITCH=-40 \
  HE_DUMP_CAMX=-3 HE_DUMP_CAMY=3 HE_DUMP_CAMZ=-5 HE_DUMP_CLOUDMODE=0 HE_DUMP_COVERAGE=0 \
  HE_DUMP_CLOUDSHADOWS=0 HE_DUMP_AA=0 HE_DUMP_RENDERPATH=0 "$@" \
  script -q $OUT/$NAME.log ./HorizonEditor > /dev/null 2>&1 &
PID=$!
T0=$(date +%s)
until [ -s $OUT/$NAME.bmp ] && /usr/bin/grep -q "frame dumped" $OUT/$NAME.log 2>/dev/null || ! kill -0 $PID 2>/dev/null; do
  sleep 1
  if [ $(( $(date +%s) - T0 )) -gt 400 ]; then echo "TIMEOUT $NAME"; pkill -9 -P $PID; break; fi
done
sleep 4
kill -0 $PID 2>/dev/null && { pkill -9 -P $PID 2>/dev/null; kill -9 $PID 2>/dev/null; }
echo "$NAME secs=$(( $(date +%s) - T0 )) bmp=$( [ -s $OUT/$NAME.bmp ] && echo yes || echo no )"
/usr/bin/grep -aE "failed|Session summary" $OUT/$NAME.log | sed 's/\x1b\[[0-9;]*m//g'
