#!/bin/zsh
# usage: run.sh <rounds> ; Chain+Impostor pass breakdown at STANDARD RenderScale (no --set RenderScale)
S=/private/tmp/auto-lod-step13
R=/Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-auto-lod-und-sichtbarkeitsgetrieb
cap() { # label scene mode
  local label=$1 scene=$2 mode=$3
  local args=(--project $S/proj/FoliageTest.heproj --label $label --out $S/raw --cfgdir $S/cfg_$label
      --editor $S/Editor/HorizonEditor --cam 0,2.5,-24,3.14159,-0.15 --env HE_SKY_TIME=40000 --scene $scene --warmup 300 --frames 600)
  if [[ $mode == pass ]]; then args+=(--detailed); else args+=(--no-counters); fi
  echo "$label load: $(uptime | sed 's/.*load/load/')  locked: $(/usr/sbin/ioreg -n Root -d1 | grep -c CGSSessionScreenIsLocked)  batt: $(pmset -g batt | grep -o 'Battery Power\|AC Power')" | tee -a $S/raw/conditions.txt
  caffeinate -d -u -i python3 $R/scripts/he_perf_capture.py "${args[@]}"
  echo "$label rc=$?"
}
for round in $(seq 1 ${1:-2}); do
  for v in imp imp_nocloud imp_dome ground ground_nocloud imp_noshadow; do
    cap ${v}_pass_$round $S/scenes/$v.hescene pass
  done
  for v in imp imp_nocloud ground; do
    cap ${v}_fps_$round $S/scenes/$v.hescene fps
  done
done
echo ALLDONE
