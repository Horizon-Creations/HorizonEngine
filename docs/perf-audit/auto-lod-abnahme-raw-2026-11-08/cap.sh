#!/bin/zsh
# usage: cap.sh <label> <scene> <mode: pass|fps>
S=/private/tmp/auto-lod-abnahme
R=/Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-auto-lod-und-sichtbarkeitsgetrieb
label=$1; scene=$2; mode=$3; PROJ=${4:-$S/proj}
args=(--project $PROJ/FoliageTest.heproj --label $label --out $S/raw --cfgdir $S/cfg_$label
      --editor $S/Editor/HorizonEditor --cam 0,2.5,-24,3.14159,-0.15 --env HE_SKY_TIME=40000 --scene $scene --warmup 300 --frames 600)
if [[ $mode == pass ]]; then args+=(--detailed --set RenderScale=2); else args+=(--no-counters); fi
echo "$label load: $(uptime | sed 's/.*load/load/')  gpu: $(/usr/sbin/ioreg -r -c IOAccelerator -d 1 | grep -o '"Device Utilization %"=[0-9]*')  locked: $(/usr/sbin/ioreg -n Root -d1 | grep -c CGSSessionScreenIsLocked)  batt: $(pmset -g batt | grep -o 'Battery Power\|AC Power')" | tee -a $S/raw/conditions.txt
caffeinate -d -u -i python3 $R/scripts/he_perf_capture.py "${args[@]}"
echo "$label rc=$?"
cp $S/Editor/HorizonEngine.log $S/raw/$label.log
