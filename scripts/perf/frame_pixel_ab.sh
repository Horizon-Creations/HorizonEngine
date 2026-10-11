#!/bin/zsh
# Metal pixel A/B for changes in how a frame is extracted and encoded (Thema 162, Schritt 2 and 4).
#
#   scripts/perf/frame_pixel_ab.sh <outdir> [<tree>] [frame|shadow|all]
#
# Renders the witness set below with the deployed editor of <tree> (default: this checkout's
# out/deploy/Editor) and prints "<name> <md5 of the PNG>" per shot. Run it once per build and
# compare the two lists; run it TWICE on one build first, the md5s of the second run are the
# noise floor (every shot here is bit-reproducible: HE_SKY_TIME pins the sky clock, AA is off,
# the render path is explicit, the config dir is private).
#
# The third argument picks the set: `frame` (the default, 16 shots, Schritt 2: local shadows,
# decals, foliage, instanced cascade shadows, the odd render scale), `shadow` (Schritt 4: the
# shadow pass's special cases, see below) or `all`.
#
# To compare two builds without a second worktree, keep the OLD one aside before you rebuild:
#   cp -R out/deploy/Editor /tmp/ab/old/out/deploy/Editor   (+ scripts/he_shot.py to /tmp/ab/old/scripts/)
#   scripts/perf/selfcontain_deploy_copy.sh /tmp/ab/old/out/deploy/Editor
# (a plain copy still loads the dylibs of the build tree, see that script), then pass /tmp/ab/old
# as <tree>. Prove which build a copy holds with a symbol only one of them has (`nm`).
#
# What the shots frame (they are the subjects the dump scenes were written for):
#   ls_*     one shadow-casting point/spot light, a caster cube and a floor at y~199
#   decal_*  a red decal projector over a floor (the deferred path projects it)
#   foliage_* a field of 2000 foliage instances
#   shadinst_* a row of seven instanced cubes and their cascade shadows
#   odd_*    the same at render scale 0.51: the scene is 653x367 and SSAO runs at 326x183, a
#            different ratio, so the extractor's aspect-dependent tail runs between the cascade
#            pass and the scene pass (at a round scale the half-resolution ratio is the same and
#            the tail never runs)
# The `shadow` set (what the shadow pass depends on; its shots must be DIFFERENT images, look at them):
#   dn_<t>_*   the day-night cycle at t = 0.00 (moon) 0.20 0.25 0.30 0.50 0.70 0.75 0.90 (moon): the
#              sun moves every frame, the walk must follow it. dn_off_* has the cycle OFF
#              (HE_DUMP_DAYNIGHT=0, needs an editor built after Schritt 4), dn_overcast_* full
#              cloud cover (no direct light, so no cascades), nosky_* no environment at all
#   casc*_*    the cascade fit: 1, 2, 3 cascades, a short distance, a small map, the camera near, far
#              and with the row across the first split (HE_DUMP_SHADOW=distance,cascades,res,lambda)
#   ls_*_day_* a point/spot light AND the sun casting at once (cascades and local layers in one pass)
#   odd_casc2  two cascades at the odd render scale
# GI and the low-resolution clouds are NOT here: they are not bit-reproducible.
set -u
OUT=${1:?usage: frame_pixel_ab.sh <outdir> [<tree>] [frame|shadow|all]}
TREE=${2:-${0:A:h:h:h}}
SET=${3:-frame}
mkdir -p "$OUT"
export HE_SKY_TIME=30
export HE_CONFIG_DIR=$(mktemp -d /tmp/he_frame_ab_cfg.XXXXXX)
export HE_COLLAB_OFFLINE=1

run() {
    local name=$1; shift
    rm -f "$OUT/$name.png"
    python3 "$TREE/scripts/he_shot.py" "$OUT/$name.png" "$@" AA=0 RHI=Metal >/dev/null 2>&1
    if [[ -s "$OUT/$name.png" ]]; then
        print -r -- "$name $(/sbin/md5 -q "$OUT/$name.png")"
    else
        print -r -- "$name MISSING"
    fi
}

LS="TOD=0 COVERAGE=0 CAMY=206 CAMZ=2 PITCH=-40"
DC="TOD=0.5 COVERAGE=0.3 CAMY=5 CAMZ=2 PITCH=-30"
FO="TOD=0.5 COVERAGE=0.3 CAMX=0 CAMY=40 CAMZ=-120 PITCH=-20"
SI="TOD=0.35 COVERAGE=0.3 CAMY=5 PITCH=-18"

if [[ $SET == frame || $SET == all ]]; then
run ls_point_fwd        LOCALSHADOW=point RENDERPATH=0 ${=LS}
run ls_point_def        LOCALSHADOW=point RENDERPATH=1 ${=LS}
run ls_spot_fwd         LOCALSHADOW=spot  RENDERPATH=0 ${=LS}
run ls_spot_def         LOCALSHADOW=spot  RENDERPATH=1 ${=LS}
run decal_def           DECALTEST=1 RENDERPATH=1 ${=DC}
run decal_fwd           DECALTEST=1 RENDERPATH=0 ${=DC}
run foliage_fwd         FOLIAGETEST=2000 RENDERPATH=0 ${=FO}
run foliage_def         FOLIAGETEST=2000 RENDERPATH=1 ${=FO}
run shadinst_fwd        SHADOWINSTTEST=1 RENDERPATH=0 ${=SI}
run shadinst_def        SHADOWINSTTEST=1 RENDERPATH=1 ${=SI}
run odd_shadinst_fwd    SHADOWINSTTEST=1 RENDERSCALE=0.51 RENDERPATH=0 SSAO=1 ${=SI}
run odd_shadinst_def    SHADOWINSTTEST=1 RENDERSCALE=0.51 RENDERPATH=1 SSAO=1 ${=SI}
run odd_dof_fwd         DOFTEST=1 DOF=0 RENDERSCALE=0.51 RENDERPATH=0 SSAO=1 TOD=0.5 PITCH=0 COVERAGE=0.3
run odd_ls_point_fwd    LOCALSHADOW=point RENDERSCALE=0.51 RENDERPATH=0 SSAO=1 ${=LS}
run odd_ls_spot_fwd     LOCALSHADOW=spot  RENDERSCALE=0.51 RENDERPATH=0 SSAO=1 ${=LS}
run odd_shadinst_nossao SHADOWINSTTEST=1 RENDERSCALE=0.51 RENDERPATH=0 SSAO=0 ${=SI}
fi

if [[ $SET == shadow || $SET == all ]]; then
SB="COVERAGE=0.3 CAMY=5 PITCH=-18"
# Day-night: the cycle is on in every dump, TOD moves the sun (0 and 0.9 are the moon's hours).
for t in 0 20 25 30 50 70 75 90; do
    run dn_${t}_fwd     SHADOWINSTTEST=1 RENDERPATH=0 TOD=$(printf '0.%02d' $t) ${=SB}
done
run dn_30_def           SHADOWINSTTEST=1 RENDERPATH=1 TOD=0.30 ${=SB}
run dn_70_def           SHADOWINSTTEST=1 RENDERPATH=1 TOD=0.70 ${=SB}
run dn_off_fwd          SHADOWINSTTEST=1 RENDERPATH=0 DAYNIGHT=0 TOD=0.90 ${=SB}
run dn_off_def          SHADOWINSTTEST=1 RENDERPATH=1 DAYNIGHT=0 TOD=0.90 ${=SB}
run dn_overcast_fwd     SHADOWINSTTEST=1 RENDERPATH=0 TOD=0.30 COVERAGE=1 CAMY=5 PITCH=-18
run nosky_fwd           SHADOWINSTTEST=1 RENDERPATH=0 NOSKY=1 TOD=0.35 ${=SB}
# Cascades: the project's shadow settings, the camera against the splits (3 cascades over 60 m
# split at about 10, 24 and 60 m; the row of cubes is 12 m ahead of a camera at z=0).
SC="SHADOWINSTTEST=1 RENDERPATH=0 TOD=0.35 ${=SB}"
run casc1_fwd           ${=SC} SHADOW=60,1,2048,0.5
run casc2_fwd           ${=SC} SHADOW=60,2,2048,0.5
run casc3_fwd           ${=SC} SHADOW=60,3,2048,0.5
run casc3_def           SHADOWINSTTEST=1 RENDERPATH=1 TOD=0.35 ${=SB} SHADOW=60,3,2048,0.5
run casc3_res512_fwd    ${=SC} SHADOW=60,3,512,0.5
run casc3_lambda1_fwd   ${=SC} SHADOW=60,3,2048,1
run casc3_near_fwd      ${=SC} SHADOW=60,3,2048,0.5 CAMZ=-5
run casc3_far_fwd       ${=SC} SHADOW=60,3,2048,0.5 CAMZ=15
run casc3_split_fwd     ${=SC} SHADOW=60,3,2048,0.5 YAW=40
# The sun and a local light in one shadow pass, and two cascades at the odd render scale.
run ls_point_day_fwd    LOCALSHADOW=point RENDERPATH=0 TOD=0.35 COVERAGE=0 CAMY=206 CAMZ=2 PITCH=-40
run ls_spot_day_def     LOCALSHADOW=spot  RENDERPATH=1 TOD=0.35 COVERAGE=0 CAMY=206 CAMZ=2 PITCH=-40
run odd_casc2_fwd       ${=SC} SHADOW=60,2,2048,0.5 RENDERSCALE=0.51 SSAO=1
fi
