#!/bin/zsh
# usage: run_ab.sh <rounds> <mode: pass|fps> [tag]   ABAB at STANDARD RenderScale, Chain+Impostor, card A (double-faced) vs B (single-faced)
S=/private/tmp/auto-lod-14
R=/Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-auto-lod-und-sichtbarkeitsgetrieb
rounds=$1; mode=$2; tag=${3:-}
for round in $(seq 1 $rounds); do
  for v in ${=VARS:-A B}; do
    label=${v}${tag}_${mode}_$round
    args=(--project $S/proj$v/FoliageTest.heproj --label $label --out $S/raw --cfgdir $S/cfg_$label
      --editor $S/Editor/HorizonEditor --cam 0,2.5,-24,3.14159,-0.15 --env HE_SKY_TIME=40000 --scene $S/imp$v.hescene --warmup 300 --frames 600)
    if [[ $mode == pass ]]; then args+=(--detailed); else args+=(--no-counters ${=EXTRA}); fi
    echo "$label load: $(uptime | sed 's/.*load/load/')  locked: $(/usr/sbin/ioreg -n Root -d1 | grep -c CGSSessionScreenIsLocked)  batt: $(pmset -g batt | grep -o 'Battery Power\|AC Power')" | tee -a $S/raw/conditions.txt
    caffeinate -d -u -i python3 $R/scripts/he_perf_capture.py "${args[@]}" | tail -1 | cut -c1-250
  done
done
echo ALLDONE
