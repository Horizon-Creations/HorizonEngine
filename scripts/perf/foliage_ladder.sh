#!/bin/zsh
# Foliage baseline ladder (Thema 163, Schritt 2c): one he_perf_capture run per
# instance count, on a scene with ONE foliage layer and no entity per plant.
#
#   scripts/perf/foliage_ladder.sh <project.heproj> <outdir> <label-prefix> <n>...
#   e.g. scripts/perf/foliage_ladder.sh /tmp/fol_proj/Test/Test.heproj \
#            docs/perf-audit/raw-foliage base 10000 100000 500000
#
# Scenes come from scripts/perf/gen_reference_world.py --foliage (a flat
# EXTENT m square, the layer's density derived from N, the built-in cube).
# The project's startup scene is OVERWRITTEN by he_perf_capture.py --scene, so
# pass a private copy of a project, never one you work in.
#
# Extra env: DIST (drawDistance in m, default 1e6 = every instance in range;
# 100 puts ~20 % of a 400 m field in range with the camera in the middle),
# EXTENT (default 400), CAM (default 0,30,0,0,-0.2: in the middle of the
# field, looking along -Z), WARMUP (60), FRAMES (180), TIMEOUT, MESH (cube|
# sphere), EDITOR=<binary> (A/B), SCENES (scene cache dir), TAG (extra word in
# the label, e.g. dist100), EXTRA_ENV="KEY=VAL KEY=VAL" (handed to the editor, e.g.
# HE_FOLIAGE_CLUSTERS=0 for the per-instance path of the same binary, or
# HE_FOLIAGE_STATS=1 for the cluster counters in the log). Each run writes <label>.conditions.txt next to the
# capture: load, screen lock, low-power mode and GPU use before and after.
set -u
REPO=${0:A:h:h:h}
PROJ=$1; OUT=$2; PREFIX=$3; shift 3
WARMUP=${WARMUP:-60}; FRAMES=${FRAMES:-180}; TIMEOUT=${TIMEOUT:-1200}
EXTENT=${EXTENT:-400}; DIST=${DIST:-1000000}; CAM=${CAM:-0,30,0,0,-0.2}
MESH=${MESH:-cube}; TAG=${TAG:-}
SCENES=${SCENES:-/tmp/fol_scenes}
mkdir -p "$OUT" "$SCENES"
EDARG=()
[[ -n ${EDITOR:-} ]] && EDARG=(--editor "$EDITOR")
for kv in ${=EXTRA_ENV:-}; do EDARG+=(--env "$kv"); done

conditions() {
    echo "  uptime:      $(uptime | sed 's/.*load/load/')"
    echo "  locked:      $(/usr/sbin/ioreg -n Root -d1 | grep -o '"CGSSessionScreenIsLocked"=[A-Za-z]*' || echo none)"
    echo "  lowpower:    $(/usr/bin/pmset -g | awk '/lowpowermode/ {print $2}')"
    echo "  gpu:         $(/usr/sbin/ioreg -r -c IOAccelerator -d 1 | grep -o '"Device Utilization %"=[0-9]*' | head -1)"
    echo "  power:       $(/usr/bin/pmset -g batt | head -1)"
    echo "  busy procs:  $(ps -Ao pcpu,comm | sort -rn | head -4 | awk '{printf "%s%% %s; ", $1, $2}')"
}

for n in "$@"; do
    scene=$SCENES/fol_${n}_${EXTENT}_${MESH}_d${DIST}.hescene
    [[ -f $scene ]] || python3 -I "$REPO/scripts/perf/gen_reference_world.py" --count 0 \
        --foliage $n --foliage-extent $EXTENT --foliage-distance $DIST --foliage-mesh $MESH \
        --template "$REPO/docs/perf-audit/scenes/landscape_noclouds.hescene" --out "$scene"
    label=${PREFIX}-${n}${TAG:+-$TAG}
    cond=$OUT/$label.conditions.txt
    { echo "== $label  $(date '+%Y-%m-%d %H:%M:%S')"; echo "before:"; conditions; } | tee "$cond"
    python3 -I "$REPO/scripts/he_perf_capture.py" --project "$PROJ" --label "$label" --out "$OUT" \
        --scene "$scene" --warmup $WARMUP --frames $FRAMES --no-counters --timeout $TIMEOUT \
        --cam $CAM $EDARG
    rc=$?
    { echo "after (rc=$rc):"; conditions; } | tee -a "$cond"
    grep -hE 'HE_DUMP_FOLIAGETEST|Foliage' "$OUT/$label.log" 2>/dev/null | head -3 | sed 's/^/   /'
done
