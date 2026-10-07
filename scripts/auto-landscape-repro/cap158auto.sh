#!/bin/zsh
# Thema 158, Schritt 5: the auto landscape material witness on macOS (Metal + OpenGL).
#
#   scripts/auto-landscape-repro/cap158auto.sh OUTDIR MODE [RHI...] [-- KEY=VAL ...]
#
# MODE = 1 | nobomb | masks | ground (HE_DUMP_AUTOLAND, see EditorApplication.cpp),
# RHI defaults to "Metal OpenGL". Extra HE_DUMP_* overrides after "--", e.g.
# "-- PITCH=-30 CAMX=-120 CAMY=360" for the oblique look.
#
# Top-down by default (pitch -89 from y=400, the 128 m terrain at y=300 fills
# the frame height), clouds/GI/SSAO/SSR/AA/bloom/DoF/motion blur off, forward,
# sky time pinned: two runs of one binary are bit-identical, so any difference
# between backends is the backends. Each capture gets a private HE_CONFIG_DIR
# (a clean exit rewrites config.json) and its own log via script(1) (a pty, so
# the log is line-buffered even when the teardown segfaults after the dump).
set -u
OUT=$1; MODE=$2; shift 2
RHIS=()
while (( $# )) && [[ $1 != "--" ]]; do RHIS+=$1; shift; done
(( $# )) && shift
EXTRA=("$@")
(( ${#RHIS} )) || RHIS=(Metal OpenGL)

ROOT=${0:A:h:h:h}
ED=$ROOT/out/deploy/Editor
[[ -x $ED/HorizonEditor ]] || { echo "no editor at $ED"; exit 1; }
mkdir -p $OUT

for RHI in $RHIS; do
  NAME=AL$MODE-$RHI${HE_TAG:-}
  BMP=$OUT/$NAME.bmp; LOG=$OUT/$NAME.log; CFG=$OUT/cfg-$NAME
  rm -rf $BMP $CFG; mkdir -p $CFG
  ENVS=(HE_CONFIG_DIR=$CFG HE_COLLAB_OFFLINE=1 HE_SKY_TIME=30
        HE_DUMP_PATH=$BMP HE_DUMP_QUIT=1 HE_DUMP_RHI=$RHI HE_DUMP_FRAMES=16
        HE_DUMP_SKYTEST=1 HE_DUMP_TOD=0.4 HE_DUMP_COVERAGE=0 HE_DUMP_CLOUDMODE=0
        HE_DUMP_CLOUDSHADOWS=0
        HE_DUMP_CAMX=0 HE_DUMP_CAMY=400 HE_DUMP_CAMZ=0 HE_DUMP_PITCH=-89
        HE_DUMP_GI=0 HE_DUMP_SSAO=0 HE_DUMP_SSR=0 HE_DUMP_AA=0 HE_DUMP_BLOOM=0
        HE_DUMP_DOF=0 HE_DUMP_MOTIONBLUR=0 HE_DUMP_RENDERPATH=0
        HE_DUMP_AUTOLAND=$MODE)
  for kv in $EXTRA; do ENVS+=HE_DUMP_$kv; done
  ( cd $ED && env $ENVS script -q $LOG ./HorizonEditor >/dev/null 2>&1 )
  W=$(/usr/bin/grep -a "AUTOLAND witness" $LOG | head -1 | sed 's/.*witness landscape added //')
  C=$(/usr/bin/grep -a "dump counters" $LOG | tail -1 | sed 's/.*dump counters/counters/')
  E=$(/usr/bin/grep -a -c -E "\[ERROR\]|link failed|compile failed" $LOG)
  printf "%-22s bmp=%s errors=%s %s | %s\n" $NAME $([[ -s $BMP ]] && echo yes || echo NO) $E "$W" "$C"
done
