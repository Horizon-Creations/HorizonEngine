#!/bin/zsh
# World-streaming baseline ladder (Thema 153): one he_perf_capture run per
# reference-world size, then the load timings pulled out of the captured log.
#
#   scripts/perf/world_streaming_ladder.sh <project.heproj> <outdir> <label-prefix> <n>...
#   e.g. scripts/perf/world_streaming_ladder.sh /tmp/ws_proj/Test/Test.heproj \
#            docs/perf-audit/raw-streaming base 1000 10000 50000
#
# Scenes come from scripts/perf/gen_reference_world.py (no physics: the editor
# does not build Jolt bodies outside play mode, so they would only add parse
# cost). WARMUP=0 so the capture starts with the first frame after the load and
# the post-load hitches (terrain build, first-draw uploads) are in it.
# Extra env: EDITOR=<binary> (A/B), WARMUP, FRAMES, TIMEOUT, EXTENT, OFFSET.
set -u
REPO=${0:A:h:h:h}
PROJ=$1; OUT=$2; PREFIX=$3; shift 3
WARMUP=${WARMUP:-0}; FRAMES=${FRAMES:-600}; TIMEOUT=${TIMEOUT:-1200}
EXTENT=${EXTENT:-8000}; OFFSET=${OFFSET:-0,0,0}
SCENES=${SCENES:-/tmp/ws_scenes}
mkdir -p "$OUT" "$SCENES"
EDARG=()
[[ -n ${EDITOR:-} ]] && EDARG=(--editor "$EDITOR")

for n in "$@"; do
    scene=$SCENES/ref_${n}_${EXTENT}_${OFFSET//,/_}.hescene
    [[ -f $scene ]] || python3 "$REPO/scripts/perf/gen_reference_world.py" --count $n \
        --extent $EXTENT --offset $OFFSET --groups $((n / 100)) --lights 64 \
        --template "$REPO/docs/perf-audit/scenes/landscape_noclouds.hescene" --out "$scene"
    label=${PREFIX}-${n}
    echo "== $label  $(uptime | sed 's/.*load/load/')  locked=$(/usr/sbin/ioreg -n Root -d1 | grep -o '"CGSSessionScreenIsLocked"=[A-Za-z]*' || echo none)"
    python3 "$REPO/scripts/he_perf_capture.py" --project "$PROJ" --label "$label" --out "$OUT" \
        --scene "$scene" --warmup $WARMUP --frames $FRAMES --no-counters --timeout $TIMEOUT \
        --cam 0,25,90,0,-0.25 $EDARG
    echo "   rc=$?"
    grep -hE 'SceneLoadTiming|SceneOpenTiming|startup scene loaded|ThreadPool starting' "$OUT/$label.log" 2>/dev/null | sed 's/^/   /'
    head -1 "$OUT/$label.log" 2>/dev/null | cut -c1-15 | sed 's/^/   first log line at /'
done
