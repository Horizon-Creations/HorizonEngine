#!/bin/zsh
# Thema 233, step 5: group witnesses (HE_DUMP_AUTOLAND + HE_DUMP_AUTOLANDGROUP) per backend.
#
#   scripts/landscape-3-0-accept/cap233.sh OUTDIR ARRAYDIR GROUP RHI [TAG [EXTRA_ENV=VAL ...]]
#
# ARRAYDIR holds the group's three arrays (read only; Mountains: the real EngineContentCache
# Textures/Landscape, Forest/Grassland: `landscape_tex_gen <dir> --group <G>` placeholders).
# ED=<editor dir> picks another deploy (the pre-3.0 baseline). Every run has a private HOME and
# config dir, so the real cache is never touched; GI/SSAO/AA/bloom are off, two runs of one
# binary are bit-identical (cap210.sh, same pinning). Prints md5, counters, missing-asset lines.
set -u
OUT=${1:A}; SRC=${2:A}; GROUP=$3; RHI=$4; TAG=${5:-}; shift $(( $# < 5 ? $# : 5 ))
ROOT=${0:A:h:h:h}
ED=${ED:-$ROOT/out/deploy/Editor}
[[ -x $ED/HorizonEditor ]] || { echo "no editor at $ED"; exit 1; }
mkdir -p $OUT
PFX=""; [[ $GROUP != Mountains ]] && PFX="${GROUP}_"
NAME=$GROUP-$RHI${TAG:+-$TAG}
BMP=$OUT/$NAME.bmp; LOG=$OUT/$NAME.log; CFG=$OUT/cfg-$NAME; H=$OUT/home-$NAME
rm -rf $BMP $CFG $H; mkdir -p $CFG
CACHE="$H/Library/Application Support/HorizonEngine/EngineContentCache/Textures/Landscape"
mkdir -p $CACHE
for f in Albedo Normal Mask; do cp -c $SRC/T_Landscape_${PFX}${f}_Array.hasset $CACHE/ || exit 1; done
ENVS=(HOME=$H HE_CONFIG_DIR=$CFG HE_COLLAB_OFFLINE=1 HE_SKY_TIME=30
      HE_DUMP_PATH=$BMP HE_DUMP_QUIT=1 HE_DUMP_RHI=$RHI HE_DUMP_FRAMES=16
      HE_DUMP_SKYTEST=1 HE_DUMP_TOD=0.4 HE_DUMP_COVERAGE=0 HE_DUMP_CLOUDMODE=0
      HE_DUMP_CLOUDSHADOWS=0 HE_DUMP_CAMX=0 HE_DUMP_CAMY=400 HE_DUMP_CAMZ=0 HE_DUMP_PITCH=-89
      HE_DUMP_GI=0 HE_DUMP_SSAO=0 HE_DUMP_SSR=0 HE_DUMP_AA=0 HE_DUMP_BLOOM=0
      HE_DUMP_DOF=0 HE_DUMP_MOTIONBLUR=0 HE_DUMP_RENDERPATH=0 HE_DUMP_AUTOLAND=1)
[[ $GROUP != Mountains && ${ED_NOGROUP:-0} != 1 ]] && ENVS+=(HE_DUMP_AUTOLANDGROUP=$GROUP)
ENVS+=("$@")
( cd $ED && env $ENVS ./HorizonEditor > $LOG 2>&1 </dev/null )
M=$(/usr/bin/grep -a -c "Cannot load asset" $LOG)
C=$(/usr/bin/grep -a "dump counters" $LOG | tail -1 | sed 's/.*dump counters/counters/')
S=$( [[ -s $BMP ]] && /sbin/md5 -q $BMP || echo NO-BMP )
printf "%-28s md5=%s missing-logs=%s %s\n" $NAME $S $M "$C"
[[ -s $BMP ]] && sips -s format png $BMP --out ${BMP%.bmp}.png >/dev/null 2>&1
rm -rf $H
