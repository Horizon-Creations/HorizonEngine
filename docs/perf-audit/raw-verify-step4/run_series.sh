#!/bin/zsh
# Alternating before/after perf series. Usage: run_series.sh <scene> <prefix> <label...>
# label starting with B = before binary, A = after binary.
W=/Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-perf-sky-wolken-optimierungen-a1-
OUT=$W/docs/perf-audit/raw-verify-step4
mkdir -p $OUT
SCENE=$1; PFX=$2; shift 2
for L in "$@"; do
  case $L in
    B*) ED=/tmp/he_before/out/deploy/Editor/HorizonEditor ;;
    A*) ED=$W/out/deploy/Editor/HorizonEditor ;;
  esac
  EXTRA=""; [[ $L == *N* ]] && EXTRA="--env=HE_SKY_LUT=0"
  sleep 20
  COND="$(date +%H:%M:%S) $PFX-$L load=$(uptime | sed 's/.*averages: //') gpu=$(/usr/sbin/ioreg -r -c IOAccelerator -d 1 | grep -o '"Device Utilization %"=[0-9]*' | sed 's/.*=//')% $(/usr/sbin/ioreg -n Root -d1 | grep -o '"CGSSessionScreenIsLocked"=[A-Za-z]*')"
  echo "$COND" | tee -a $OUT/conditions.txt
  python3 $W/scripts/he_perf_capture.py --project /tmp/s4proj/Test/Test.heproj --label $PFX-$L \
    --out $OUT --detailed --scene $W/docs/perf-audit/scenes/$SCENE --cam 0,25,90,0,-0.25 \
    --cfgdir /tmp/s4_perf_cfg --editor $ED $EXTRA
  echo "  rc=$?"
  rm -f $OUT/$PFX-$L.profile.json
done
