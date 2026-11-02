#!/bin/zsh
# jobs23b.sh <rounds>: (Thema 237 step 3; /tmp/he237/s3 = experiment build with HE_EXP_BOMBV, s4 = the fix)
# before (s3 build, HE_EXP_BOMBV=0 = the shipped variant shader), after (s4 = the fix), oracle (s3, =1 variants off), interleaved
T=/Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-bug-allgemeine-performance-regres/scripts/perf/landscape_regression_tools
S=/tmp/he237/scenes/catania_base.hescene
quiet() { local n=0; while [ $n -lt 2 ]; do if pgrep -x -q ninja || pgrep -x -q clang || pgrep -x -q "clang\+\+" || pgrep -x -q cc1; then n=0; else n=$((n+1)); fi; sleep 10; done; }
for r in $(seq 1 $1); do
  quiet; HE_EXP_BOMBV=0 zsh $T/cell.sh d23before-base-det-r$r /tmp/he237/s3/Editor $S Metal det
  quiet; zsh $T/cell.sh d23after-base-det-r$r /tmp/he237/s4/Editor $S Metal det
  quiet; HE_EXP_BOMBV=1 zsh $T/cell.sh d23oracle-base-det-r$r /tmp/he237/s3/Editor $S Metal det
done
echo JOBS23B_DONE
