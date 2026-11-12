#!/bin/zsh
cd /private/tmp/auto-lod-14
for r in 1 2; do
  for v in V A W T; do
    [[ $v == A && $r == 1 ]] && continue
    echo "xt${v}$r load: $(uptime | sed 's/.*load/load/')" >> raw/conditions.txt
    ./xt.sh $v xt${v}$r
    python3 -I encsum.py x_xt${v}$r.xml > enc_xt${v}$r.txt 2>&1
  done
done
echo XTALLDONE
