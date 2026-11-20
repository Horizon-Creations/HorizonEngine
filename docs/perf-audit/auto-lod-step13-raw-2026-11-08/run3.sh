#!/bin/zsh
S=/private/tmp/auto-lod-step13
R=/Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-auto-lod-und-sichtbarkeitsgetrieb
for round in 1 2 3; do for v in imp imp_cq0 imp_allcheap; do
  label=${v}_vs_c$round
  echo "$label $(uptime | sed 's/.*load/load/')" >> $S/raw/conditions.txt
  caffeinate -d -u -i python3 $R/scripts/he_perf_capture.py --project $S/proj/FoliageTest.heproj --label $label --out $S/raw --cfgdir $S/cfg_$label --editor $S/Editor/HorizonEditor --cam 0,2.5,-24,3.14159,-0.15 --env HE_SKY_TIME=40000 --scene $S/scenes/$v.hescene --warmup 300 --frames 600 --no-counters --vsync keep >/dev/null
done; done
echo ALLDONE3
