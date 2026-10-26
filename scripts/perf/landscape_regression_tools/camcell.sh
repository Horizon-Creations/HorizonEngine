#!/bin/zsh
# camcell.sh <label> <deploy-dir> <scene> <rhi> <mode> <cam> [extra...]
L=$1; D=$2; S=$3; R=$4; M=$5; CAM=$6; shift 6
EXP=~/HorizonEngineProjects/Catania/Export/Development/HEProject.app/Contents/Resources/config.json
cd /Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-bug-allgemeine-performance-regres
FLAG=""; [ "$M" = det ] && FLAG="--detailed"
python3 -I scripts/perf/landscape_regression.py --label "$L" --editor $D/HorizonEditor --project /tmp/he237/catania/HEProject.heproj --scene $S --export-config $EXP --cam $CAM --rhi $R --scale 1.86 $FLAG --out docs/perf-audit/raw-landscape-regression "$@" 2>&1 | tail -2
