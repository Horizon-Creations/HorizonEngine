#!/bin/zsh
# Metal pixel A/B for changes in how a frame is extracted and encoded (Thema 162, Schritt 2).
#
#   scripts/perf/frame_pixel_ab.sh <outdir> [<tree>]
#
# Renders the witness set below with the deployed editor of <tree> (default: this checkout's
# out/deploy/Editor) and prints "<name> <md5 of the PNG>" per shot. Run it once per build and
# compare the two lists; run it TWICE on one build first, the md5s of the second run are the
# noise floor (every shot here is bit-reproducible: HE_SKY_TIME pins the sky clock, AA is off,
# the render path is explicit, the config dir is private).
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
# GI and the low-resolution clouds are NOT here: they are not bit-reproducible.
set -u
OUT=${1:?usage: frame_pixel_ab.sh <outdir> [<tree>]}
TREE=${2:-${0:A:h:h:h}}
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
