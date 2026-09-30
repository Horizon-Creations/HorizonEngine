#!/bin/zsh
# A/B measurement of the parallel_for rework (Thema 102, B2) under artificial CPU
# load, the same editor loop as perf audit step 3 (N1/N2).
#
#   scripts/perf/b2_ab_load.sh <before-editor> <after-editor> <out-dir> [rounds] [load-procs]
#
# <before/after-editor> are HorizonEditor binaries inside COPIED deploy dirs
# (cp -R out/deploy/Editor /tmp/x), so both builds can be measured alternately.
# Runs `rounds` ABAB pairs with `load-procs` load loops (default hw.ncpu), then
# one pair without the artificial load. uptime is logged around every run; the
# load loops are this script's own children and die with it.
#
# B2_LOAD picks the load: `yes` (default, busy loops, saturating and steady) or
# `compile` (scripts/perf/compile_load.sh, recompiling a real engine TU, the
# audit's "other instances are building"; needs B2_BUILD = a configured build
# dir). The job-system stall showed up under the bursty compile-like load, not
# under steady `yes` saturation (Thema 102, step 2).
set -u
BEFORE=$1; AFTER=$2; OUT=$3; ROUNDS=${4:-2}; NLOAD=${5:-$(/usr/sbin/sysctl -n hw.ncpu)}
REPO=${0:A:h:h:h}
PROJ=${B2_PROJECT:-/tmp/b2proj/Test/Test.heproj}
LOADKIND=${B2_LOAD:-yes}
mkdir -p "$OUT"
LOG=$OUT/series.log

pids=()
stop_load() {
  for p in $pids; do pkill -TERM -P $p 2>/dev/null; kill $p 2>/dev/null; done; pids=()
}
trap stop_load EXIT INT TERM
start_load() {
  if [[ $LOADKIND == compile ]]; then
    "$REPO/scripts/perf/compile_load.sh" "${B2_BUILD:?B2_BUILD must name a configured build dir}" $NLOAD & pids+=($!)
  else
    for i in $(seq 1 $NLOAD); do yes > /dev/null & pids+=($!); done
  fi
}

cond() {
  echo "[$(date +%H:%M:%S)] $1 | $(uptime | sed 's/.*load averages*: //') | $(/usr/sbin/ioreg -n Root -d1 | grep -o '"CGSSessionScreenIsLocked"=[A-Za-z]*') | $(pmset -g | grep -o 'lowpowermode *[0-9]')" >> "$LOG"
}

run() {  # label editor
  cond "start $1"
  python3 "$REPO/scripts/he_perf_capture.py" --project "$PROJ" --out "$OUT" \
    --cam 0,25,90,0,-0.25 --scene "$REPO/docs/perf-audit/scenes/landscape.hescene" \
    --cfgdir /tmp/b2_perf_cfg --label "$1" --editor "$2" >> "$LOG" 2>&1 &
  local cap=$!
  # Proof of which engine code this run measured: the libHorizonCore the live
  # editor mapped (must lie in the same copied dir as the editor binary).
  local pid="" n=0
  while [[ -z $pid ]] && (( n < 120 )) && kill -0 $cap 2>/dev/null; do
    pid=$(pgrep -f "^${2:A}$" | head -1); (( n++ )); sleep 1
  done
  [[ -n $pid ]] && sleep 5 && echo "  core: $(vmmap $pid 2>/dev/null | grep -o '/[^ ]*libHorizonCore.dylib' | head -1)" >> "$LOG"
  wait $cap
  echo "  rc=$?" >> "$LOG"
  cond "end   $1"
}

# Local builds carry absolute rpaths into the build tree; without this both
# copies would load the build tree's current dylibs (see the helper).
"$REPO/scripts/perf/selfcontain_deploy_copy.sh" "${BEFORE:h}" >> "$LOG"
"$REPO/scripts/perf/selfcontain_deploy_copy.sh" "${AFTER:h}" >> "$LOG"

cond "series begin, $NLOAD $LOADKIND loops, $ROUNDS ABAB rounds"
start_load
sleep 60   # let the load average climb before the first capture
for r in $(seq 1 $ROUNDS); do   # order flips every round so drift in the background load cancels
  if (( r % 2 )); then
    run "L${r}-before-load" "$BEFORE"; run "L${r}-after-load" "$AFTER"
  else
    run "L${r}-after-load" "$AFTER"; run "L${r}-before-load" "$BEFORE"
  fi
done
stop_load
cond "load stopped"
sleep 60
run "Q1-before-noload" "$BEFORE"
run "Q1-after-noload"  "$AFTER"
cond "series end"
echo SERIES_DONE >> "$LOG"
