#!/bin/zsh
# px_autoland.sh <name> <editor> <expmode> [autoland mode]  -> /tmp/he237s3/px/<name>.bmp
# One deterministic HE_DUMP_AUTOLAND frame (cap210 'present' recipe, private HOME, GI/AA off, so two runs of one binary
# are bit-identical). Env: RHI=Metal|OpenGL, ARR=<dir with the three T_Landscape_*_Array.hasset> (default: the real
# EngineContentCache). <expmode> is HE_EXP_BOMBV of the Thema 237 step 3 experiment build (0 = shipped variant shader,
# 1 = variants off); the fixed engine ignores it. Compare two frames with pdiff.py <a> <b> (names without .bmp).
OUT=/tmp/he237s3/px; NAME=$1; ED=$2; MODE=${4:-1}
H=$OUT/home-$NAME; CFG=$OUT/cfg-$NAME; rm -rf $H $CFG $OUT/$NAME.bmp; mkdir -p $CFG
CACHE="$H/Library/Application Support/HorizonEngine/EngineContentCache/Textures/Landscape"; mkdir -p $CACHE
SRC=${ARR:-"$HOME/Library/Application Support/HorizonEngine/EngineContentCache/Textures/Landscape"}
for f in Albedo Normal Mask; do cp -c "$SRC/T_Landscape_${f}_Array.hasset" $CACHE/; done
cp -c "$SRC/T_Landscape_Grass_Albedo.hasset" $CACHE/ 2>/dev/null
env HOME=$H HE_CONFIG_DIR=$CFG HE_COLLAB_OFFLINE=1 HE_SKY_TIME=30 HE_EXP_BOMBV=$3 \
  HE_DUMP_PATH=$OUT/$NAME.bmp HE_DUMP_QUIT=1 HE_DUMP_RHI=${RHI:-Metal} HE_DUMP_FRAMES=16 \
  HE_DUMP_SKYTEST=1 HE_DUMP_TOD=0.4 HE_DUMP_COVERAGE=0 HE_DUMP_CLOUDMODE=0 HE_DUMP_CLOUDSHADOWS=0 \
  HE_DUMP_CAMX=0 HE_DUMP_CAMY=400 HE_DUMP_CAMZ=0 HE_DUMP_PITCH=-89 \
  HE_DUMP_GI=0 HE_DUMP_SSAO=0 HE_DUMP_SSR=0 HE_DUMP_AA=0 HE_DUMP_BLOOM=0 HE_DUMP_DOF=0 HE_DUMP_MOTIONBLUR=0 HE_DUMP_RENDERPATH=0 \
  HE_DUMP_AUTOLAND=$MODE perl -e 'alarm 200; exec @ARGV' $ED > $OUT/$NAME.log 2>&1
rm -rf $H $CFG
