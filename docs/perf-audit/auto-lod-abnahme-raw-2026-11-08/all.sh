#!/bin/zsh
S=/private/tmp/auto-lod-abnahme
P=$S/proj/variants
for round in 1 2; do
 for trip in v0:$P/v0_base.hescene:$S/proj v5:$P/v5_all.hescene:$S/proj chain:$S/chain.hescene:$S/projchain impostor:$S/impostor.hescene:$S/projbake; do
  n=${trip%%:*}; rest=${trip#*:}; sc=${rest%%:*}; pj=${rest#*:}
  for mode in pass fps; do $S/cap.sh ${n}_${mode}_$round $sc $mode $pj; done
 done
done
echo ALLDONE
