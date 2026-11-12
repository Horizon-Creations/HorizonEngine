#!/bin/zsh
S=/private/tmp/auto-lod-abnahme; P=$S/proj/variants
$S/cap.sh v0_pass_3 $P/v0_base.hescene pass $S/proj
$S/cap.sh impostor_pass_3 $S/impostor.hescene pass $S/projbake
$S/cap.sh v0_fps_3 $P/v0_base.hescene fps $S/proj
$S/cap.sh impostor_fps_3 $S/impostor.hescene fps $S/projbake
echo R3DONE
