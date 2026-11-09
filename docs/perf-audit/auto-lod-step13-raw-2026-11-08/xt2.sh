#!/bin/zsh
# usage: xt2.sh <scene> <name> [extra env KEY=VAL ...]
S=/private/tmp/auto-lod-step13
scene=$1; name=$2; shift 2
cp $S/scenes/$scene.hescene $S/proj/Content/StartupScene.hescene
rm -rf $S/$name.trace
cd $S/Editor
env HE_CONFIG_DIR=$S/cfg_xt HE_SKY_TIME=40000 HE_PROFILE_CAPTURE=1200 HE_PROFILE_WARMUP=200 HE_PROFILE_COUNTERS=0 "$@" $S/Editor/HorizonEditor >/dev/null 2>&1 &
EP=$!
sleep 9
xcrun xctrace record --template 'Metal System Trace' --attach $EP --output $S/$name.trace --time-limit 6s --no-prompt >/dev/null 2>&1
wait $EP
xcrun xctrace export --input $S/$name.trace --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' -o $S/x_$name.xml >/dev/null 2>&1
xcrun xctrace export --input $S/$name.trace --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-state-intervals"]' -o $S/xs_$name.xml >/dev/null 2>&1
echo "XTDONE $name"
