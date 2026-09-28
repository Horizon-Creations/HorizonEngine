#!/bin/zsh
# A/B measurement of the parallel_for rework (Thema 102, B2) under artificial CPU
# load, the same editor loop as perf audit step 3 (N1/N2).
#
#   scripts/perf/b2_ab_load.sh <before-editor> <after-editor> <out-dir> [rounds] [load-procs]
#
# <before/after-editor> are HorizonEditor binaries inside COPIED deploy dirs
# (cp -R out/deploy/Editor /tmp/x), so both builds can be measured alternately.
# Runs `rounds` ABAB pairs with `load-procs` busy loops (`yes`, default hw.ncpu),
# then one pair without the artificial load. uptime is logged around every run;
# the busy loops are this script's own children and die with it.
set -u
BEFORE=$1; AFTER=$2; OUT=$3; ROUNDS=${4:-2}; NLOAD=${5:-$(/usr/sbin/sysctl -n hw.ncpu)}
REPO=${0:A:h:h:h}
PROJ=${B2_PROJECT:-/tmp/b2proj/Test/Test.heproj}
mkdir -p "$OUT"
LOG=$OUT/series.log

pids=()
stop_load() { for p in $pids; do kill $p 2>/dev/null; done; pids=(); }
trap stop_load EXIT INT TERM

cond() {
  echo "[$(date +%H:%M:%S)] $1 | $(uptime | sed 's/.*load averages*: //') | $(/usr/sbin/ioreg -n Root -d1 | grep -o '"CGSSessionScreenIsLocked"=[A-Za-z]*') | $(pmset -g | grep -o 'lowpowermode *[0-9]')" >> "$LOG"
}

run() {  # label editor
  cond "start $1"
  python3 "$REPO/scripts/he_perf_capture.py" --project "$PROJ" --out "$OUT" \
    --cam 0,25,90,0,-0.25 --scene "$REPO/docs/perf-audit/scenes/landscape.hescene" \
    --cfgdir /tmp/b2_perf_cfg --label "$1" --editor "$2" >> "$LOG" 2>&1
  echo "  rc=$?" >> "$LOG"
  cond "end   $1"
}

cond "series begin, $NLOAD busy loops, $ROUNDS ABAB rounds"
for i in $(seq 1 $NLOAD); do yes > /dev/null & pids+=($!); done
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
