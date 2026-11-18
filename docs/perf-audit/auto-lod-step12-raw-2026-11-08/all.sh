#!/bin/zsh
# before = pre-fix clone from step 7, after = this branch HEAD (fix 9e7a2f05b); exactly one dylib differs (libHorizonCore)
S=/private/tmp/auto-lod-step12
O=/private/tmp/auto-lod-abnahme
P=$O/proj/variants
BEF=$O/Editor
AFT=$S/Editor
for round in 1 2; do
 for trip in v0:$P/v0_base.hescene:$O/proj v5:$P/v5_all.hescene:$O/proj chain:$O/chain.hescene:$O/projchain impostor:$O/impostor.hescene:$O/projbake; do
  n=${trip%%:*}; rest=${trip#*:}; sc=${rest%%:*}; pj=${rest#*:}
  for mode in pass fps; do
   $S/cap.sh before_${n}_${mode}_$round $sc $mode $pj $BEF
   $S/cap.sh after_${n}_${mode}_$round $sc $mode $pj $AFT
  done
 done
done
echo ALLDONE
