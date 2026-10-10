#!/bin/zsh
# RenderExtractor::extract on the reference world whole against streamed cells (Thema 164, step 5).
# One he_tests process per mode, so /usr/bin/time -l's peak numbers belong to one mode; every run
# prints the machine's load before and after, because a foreign build or test next door moves the
# numbers (mark a figure taken under load as such when it goes into a document).
#
#   scripts/perf/cell_extract_bench.sh <scene.hescene> <whole|streamed>...
#   e.g. scripts/perf/cell_extract_bench.sh /tmp/ws_scenes/ref_100000_8000_0_0_0.hescene whole streamed
#
# Env for the streamed mode: HE_CELL_BENCH_CELL (m, default 512), HE_CELL_BENCH_LOAD (m, default
# 1.5 x cell), HE_CELL_BENCH_WALK (m, 0 = no walk) and HE_CELL_BENCH_SPEED (m/s, default 50).
# TESTS=<he_tests binary> to measure another build.
set -u
REPO=${0:A:h:h:h}
TESTS=${TESTS:-$REPO/out/build/release/tests/he_tests}
SCENE=$1; shift
for mode in "$@"; do
    echo "== $mode  $(basename $SCENE)  cell=${HE_CELL_BENCH_CELL:-512} load=${HE_CELL_BENCH_LOAD:-default} walk=${HE_CELL_BENCH_WALK:-0}"
    echo "   load before: $(uptime | sed 's/.*load/load/')   busiest: $(ps -Ao pcpu,comm -r | sed -n 2,6p | awk '{n=split($2,a,"/"); printf "%s %s%%; ", a[n], $1}')"
    HE_CELL_BENCH_WHOLE=$SCENE HE_CELL_BENCH_MODE=$mode \
        /usr/bin/time -l "$TESTS" --no-skip --test-case='Cell streaming bench: extract*' 2>&1 \
        | grep -E 'MESSAGE|^[a-z].*(:|settled)|passed|failed|maximum resident|peak memory footprint|real|ERROR' \
        | grep -vE '^(Test|Filters|doctest|\[doctest\])' | sed 's/^/   /'
    echo "   load after:  $(uptime | sed 's/.*load/load/')"
done
