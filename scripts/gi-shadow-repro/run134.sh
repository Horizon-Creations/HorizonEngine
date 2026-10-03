#!/bin/zsh
# Thema 134 measurement matrix on Metal: f60/f61 pairs per (case, variant) via
# cap_metal.sh, two captures at a time. The HE_GI_PROTO_* variants need a
# macos-release build carrying the prototype patch (scripts/gi-shadow-repro/
# proto134-metal.patch); the Schritt-2 variants (gtR, A, AB, ABC, ABCr1/r4,
# ABnof) run on the plain build (docs/gi-shadow-restflackern-1spp-2026-10-03.md §8).
# usage: run134.sh CASE VARIANT...      (see the two tables below)
#   then: python3 ana134.py $OUT/CASE__gt_f60.bmp $OUT/CASE__V_f60.bmp $OUT/CASE__V_f61.bmp
HERE=${0:A:h}
typeset -A CASE VAR
# Cases: config template | scene/camera/motion env
CASE[s6]="config_r6.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=1"
CASE[s05]="config_r05.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=1"
CC="HE_DUMP_CAMX=-3 HE_DUMP_CAMY=6 HE_DUMP_CAMZ=-6 HE_DUMP_PITCH=-50"   # over the standing row
CASE[c6]="config_r6.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=contact $CC"
CASE[c05]="config_r05.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=contact $CC"
CASE[cnear]="config_r05.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=contact HE_DUMP_CAMX=-7 HE_DUMP_CAMY=2.2 HE_DUMP_CAMZ=-9 HE_DUMP_PITCH=-35"
# Motion cases end at the pose of their static twin: compare them against THAT gt
# (s6 / c6), a gt rendered under motion would lag behind itself.
CASE[pan6]="config_r6.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=1 HE_DUMP_PANYAW=0.25"
CASE[move6]="config_r6.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=1 HE_DUMP_PANMOVE=0.03"
CASE[cpan6]="config_r6.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=contact $CC HE_DUMP_PANYAW=0.25"
# Occluder motion (ana_motion.py NEW OLD MOVED): NEW = s6/s05 f60, OLD = sun 0.005
# of a day earlier, MOVED = settled at OLD, last two frames at NEW (TODSTEP).
CASE[s6old]="config_r6.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=1 HE_DUMP_TOD=0.345"
CASE[s6mov]="config_r6.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=1 HE_DUMP_TODSTEP=0.005"
CASE[s05old]="config_r05.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=1 HE_DUMP_TOD=0.345"
CASE[s05mov]="config_r05.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=1 HE_DUMP_TODSTEP=0.005"
CASE[cmove6]="config_r6.json|HE_DUMP_GI=1 HE_DUMP_SHADOWINSTTEST=contact $CC HE_DUMP_PANMOVE=0.03"
# Variants: HE_GI_PROTO_* env (empty = stock 1 spp, 0.9, 3x3 box)
VAR[gt]="HE_GI_PROTO_SPP=256 HE_GI_PROTO_HIST=0.98 HE_GI_PROTO_ATROUS=-1"
VAR[stock]=""
VAR[spp2]="HE_GI_PROTO_SPP=2"
VAR[spp4]="HE_GI_PROTO_SPP=4"
VAR[strat]="HE_GI_PROTO_STRAT=1"
VAR[hist95]="HE_GI_PROTO_HIST=0.95"
VAR[at3]="HE_GI_PROTO_ATROUS=3"
VAR[at3l]="HE_GI_PROTO_ATROUS=3 HE_GI_PROTO_SIGL=2"
VAR[at2l]="HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[sat2l]="HE_GI_PROTO_STRAT=1 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[sat3l]="HE_GI_PROTO_STRAT=1 HE_GI_PROTO_ATROUS=3 HE_GI_PROTO_SIGL=2"
VAR[s2at2l]="HE_GI_PROTO_SPP=2 HE_GI_PROTO_STRAT=1 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[at2l4]="HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=4"
VAR[h95at2l]="HE_GI_PROTO_HIST=0.95 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[p2at2l]="HE_GI_PROTO_SPP=2 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[p4at2l]="HE_GI_PROTO_SPP=4 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[p4]="HE_GI_PROTO_SPP=4"   # alias of spp4 for the motion cases
VAR[at1l]="HE_GI_PROTO_ATROUS=1 HE_GI_PROTO_SIGL=2"
VAR[ctl]=""   # second stock run after a rebuild: must be byte-identical to stock
VAR[bl]="HE_GI_PROTO_BILERP=1"
VAR[blspp4]="HE_GI_PROTO_BILERP=1 HE_GI_PROTO_SPP=4"
VAR[blh95]="HE_GI_PROTO_BILERP=1 HE_GI_PROTO_HIST=0.95"
VAR[blat2l]="HE_GI_PROTO_BILERP=1 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[blp2at2l]="HE_GI_PROTO_BILERP=1 HE_GI_PROTO_SPP=2 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[blp4at2l]="HE_GI_PROTO_BILERP=1 HE_GI_PROTO_SPP=4 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[mv]="HE_GI_PROTO_BILERP=2"
VAR[mvspp4]="HE_GI_PROTO_BILERP=2 HE_GI_PROTO_SPP=4"
VAR[mvp2at2l]="HE_GI_PROTO_BILERP=2 HE_GI_PROTO_SPP=2 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[mvp4at2l]="HE_GI_PROTO_BILERP=2 HE_GI_PROTO_SPP=4 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[mvp2at2le]="HE_GI_PROTO_BILERP=2 HE_GI_PROTO_SPP=2 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2 HE_GI_PROTO_EARLY=1"
VAR[mvat2l]="HE_GI_PROTO_BILERP=2 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
VAR[blh95at2l]="HE_GI_PROTO_BILERP=1 HE_GI_PROTO_HIST=0.95 HE_GI_PROTO_ATROUS=2 HE_GI_PROTO_SIGL=2"
# Schritt 2 (the real implementation, no prototype patch): the build's own
# defaults, one name per stage so captures of successive builds sit side by side.
VAR[A]=""      # Baustein A: motion-vector bilinear history
VAR[AB]=""     # + B: 2 rays per pixel (default)
VAR[ABC]=""    # + C: edge-aware a-trous (the shipped chain)
VAR[gtR]="HE_GI_REFERENCE=1"           # the engine's own reference switch (256 rays, 0.98, no filter)
VAR[ABCr1]="HE_DUMP_GISHADOWRAYS=1"    # shipped chain at GI Shadow Quality Low
VAR[ABCr4]="HE_DUMP_GISHADOWRAYS=4"    # ... and High
VAR[ABnof]="HE_DUMP_GISHADOWFILTER=0"  # A+B, filter off (unfiltered mask)
C=$1; shift
spec=${CASE[$C]}; [ -z "$spec" ] && { echo "unknown case $C"; exit 1; }
cfg=$HERE/${spec%%|*}; envs=${spec#*|}
jobs_=()
for V in "$@"; do
  (( ${+VAR[$V]} )) || { echo "unknown variant $V"; continue; }
  for F in 60 61; do
    GI_CAP_CONFIG=$cfg $HERE/cap_metal.sh ${C}__${V}_f$F ${=envs} ${=VAR[$V]} HE_DUMP_FRAMES=$F &
    jobs_+=($!)
    if (( ${#jobs_} >= 2 )); then wait $jobs_[1]; jobs_=(${jobs_[2,-1]}); fi
  done
done
wait
