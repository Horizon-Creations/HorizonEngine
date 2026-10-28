#!/bin/zsh
# jobs22.sh: remaining Thema 237 step 2 cells, each one started only after ninja/clang have been gone for 20 s
T=/Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-bug-allgemeine-performance-regres/scripts/perf/landscape_regression_tools
S=/tmp/he237/scenes/catania_base.hescene
quiet() { local n=0; while [ $n -lt 2 ]; do if pgrep -x -q ninja || pgrep -x -q clang || pgrep -x -q "clang\+\+" || pgrep -x -q cc1; then n=0; else n=$((n+1)); fi; sleep 10; done; }
job() { quiet; zsh $T/cell.sh $1-base-det-r$2 /tmp/he237/$3/Editor $S Metal det; }
job d22oldmat 2 cur_oldmat
job d22cur 3 cur
job d22d906 3 cur_d906
job d22oldmat 3 cur_oldmat
for r in 1 2 3; do
  job d22b1010a $r b1010a
  job d22b1010a_b218 $r b1010a_b218
  job d22b1008b $r b1008b
  job d22b1008b_ac7f $r b1008b_ac7f
done
echo JOBS22_DONE
