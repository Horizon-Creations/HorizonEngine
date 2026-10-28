#!/bin/zsh
# prep.sh <name> <worktree-dir-name or path to a deploy Editor dir>  -> /tmp/he237/<name>/Editor (self-contained, no EngineContent/Textures/Landscape so the EngineContentCache arrays are used)
N=$1; SRC=$2
[ -d "$SRC/out/deploy/Editor" ] && SRC="$SRC/out/deploy/Editor"
mkdir -p /tmp/he237/$N
/usr/bin/rsync -a --exclude 'dumps' --exclude 'EngineContent/Textures/Landscape' --exclude 'HorizonEngine*.log' --exclude 'SDK' --exclude 'EngineSDK' "$SRC/" /tmp/he237/$N/Editor/
zsh /Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-bug-allgemeine-performance-regres/scripts/perf/selfcontain_deploy_copy.sh /tmp/he237/$N/Editor
