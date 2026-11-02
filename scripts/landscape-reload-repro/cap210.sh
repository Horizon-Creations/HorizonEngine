#!/bin/zsh
# Thema 210, Schritt 1: hexagons on the auto landscape after its texture arrays
# arrive late. The counter-probe for the cause (docs/landscape-hex-reload-ursache-2026-10-18.md).
#
#   scripts/landscape-reload-repro/cap210.sh OUTDIR ARRAYDIR CASE [RHI...]
#
# ARRAYDIR holds the three T_Landscape_*_Array.hasset (read only, e.g. the real
# EngineContentCache/Textures/Landscape). CASE:
#   present   the arrays are in the cache before the editor starts  (first load)
#   early     they arrive before the scene is built                 (HE_DUMP_AUTOLANDARRIVEAT=early)
#   mid       they arrive halfway through the settle frames, after
#             the backend resolved the array slots once              (the reload)
#   absent    they never arrive                                      (white arrays)
#   absentnobomb  absent, with the "Texture Bombing" switch off      (is the pattern the bombing?)
#   presentnobomb present, bombing off
#
# Needs a deploy WITHOUT the arrays in EngineContent (this worktree's out/deploy/Editor:
# EditorDeps/EngineContent/Textures/Landscape is not in git). Every capture runs with a
# private HOME, so GlobalState::engineContentCacheDir() is an empty per-run folder and the
# human's real cache is only ever read. Camera/sky/effects pinned like cap158auto.sh:
# GI=0, so two runs of one binary are bit-identical.
set -u
OUT=${1:A}; SRC=${2:A}; CASE=$3; shift 3
RHIS=("$@"); (( ${#RHIS} )) || RHIS=(Metal OpenGL)

ROOT=${0:A:h:h:h}
ED=$ROOT/out/deploy/Editor
[[ -x $ED/HorizonEditor ]] || { echo "no editor at $ED"; exit 1; }
[[ -e $ED/EngineContent/Textures/Landscape/T_Landscape_Albedo_Array.hasset ]] &&
  { echo "deploy has the arrays in EngineContent: nothing would arrive late"; exit 1; }
for f in Albedo Normal Mask; do [[ -s $SRC/T_Landscape_${f}_Array.hasset ]] || { echo "no $f array in $SRC"; exit 1; }; done
mkdir -p $OUT

MODE=1; [[ $CASE == *nobomb ]] && MODE=nobomb
for RHI in $RHIS; do
  NAME=$CASE-$RHI${HE_TAG:-}
  BMP=$OUT/$NAME.bmp; LOG=$OUT/$NAME.log; CFG=$OUT/cfg-$NAME; H=$OUT/home-$NAME
  rm -rf $BMP $CFG $H; mkdir -p $CFG
  CACHE="$H/Library/Application Support/HorizonEngine/EngineContentCache/Textures/Landscape"
  mkdir -p $CACHE
  ENVS=(HOME=$H HE_CONFIG_DIR=$CFG HE_COLLAB_OFFLINE=1 HE_SKY_TIME=30
        HE_DUMP_PATH=$BMP HE_DUMP_QUIT=1 HE_DUMP_RHI=$RHI HE_DUMP_FRAMES=16
        HE_DUMP_SKYTEST=1 HE_DUMP_TOD=0.4 HE_DUMP_COVERAGE=0 HE_DUMP_CLOUDMODE=0
        HE_DUMP_CLOUDSHADOWS=0
        HE_DUMP_CAMX=0 HE_DUMP_CAMY=400 HE_DUMP_CAMZ=0 HE_DUMP_PITCH=-89
        HE_DUMP_GI=0 HE_DUMP_SSAO=0 HE_DUMP_SSR=0 HE_DUMP_AA=0 HE_DUMP_BLOOM=0
        HE_DUMP_DOF=0 HE_DUMP_MOTIONBLUR=0 HE_DUMP_RENDERPATH=0
        HE_DUMP_AUTOLAND=$MODE)
  case $CASE in
    present*) for f in Albedo Normal Mask; do cp -c $SRC/T_Landscape_${f}_Array.hasset $CACHE/; done ;;
    early)    ENVS+=(HE_DUMP_AUTOLANDARRIVE=$SRC HE_DUMP_AUTOLANDARRIVEAT=early) ;;
    mid)      ENVS+=(HE_DUMP_AUTOLANDARRIVE=$SRC HE_DUMP_AUTOLANDARRIVEAT=mid) ;;
    absent*)  ;;
    *) echo "unknown case $CASE"; exit 1 ;;
  esac
  ( cd $ED && env $ENVS script -q $LOG ./HorizonEditor >/dev/null 2>&1 </dev/null )
  A=$(/usr/bin/grep -a -c "HE_DUMP_AUTOLANDARRIVE (.*loaded yes (2048x2048, 5 layers" $LOG)
  M=$(/usr/bin/grep -a -c "Cannot load asset 'Engine/Textures/Landscape/T_Landscape_.*_Array" $LOG)
  C=$(/usr/bin/grep -a "dump counters" $LOG | tail -1 | sed 's/.*dump counters/counters/')
  S=$( [[ -s $BMP ]] && /sbin/md5 -q $BMP || echo NO-BMP )
  printf "%-20s md5=%s arrived=%s missing-logs=%s %s\n" $NAME $S $A $M "$C"
  rm -rf $H   # 320 MiB of copied arrays per run
done
