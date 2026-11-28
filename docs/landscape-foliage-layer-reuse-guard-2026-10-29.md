# Landscape foliage layer: reuse guard covers every hand-tunable field (Thema 327, step 11)

Review note from run review-327-3: the pristine-starter guard in `applyFoliageSet()`
checked name/mesh/material/mask/LODs/weightLayer and (step 10) slope/height, but not
the scalar fields. A starter layer whose only hand-tuning was e.g. `density` or
`castsShadow` counted as pristine and was overwritten.

## What changed

- `src/HE_Editor/TerrainTools.cpp:325-348`: the `&&` chain now also requires
  `density, seed, minScale, maxScale, drawDistance, bucketSize, castsShadow,
  contributesAO, shadowDistance, lodHysteresis, visible` to equal `FoliageLayer{}`
  (`==`, no epsilon: the starter has never been written). Comment updated.
- `tests/test_terrain_tools_ui.cpp` (end of file): the step-10 test is renamed
  "...whose hand-tunable fields are default" and has one SUBCASE per new field plus
  the existing pristine control; the handTuned branch CHECKs every field stays as set.

## Decisions

- `visible` and `seed` were added although the note did not list them: both are
  hand-tunable and `applyFoliageSet` never writes them, so a reused starter would
  silently keep a hidden/odd-seed state the author did not mean for the set.
- Left out: `dirty` (apply sets it), `maskRes` (only meaningful with a mask, already checked).
- Guard stays above the write block (density/scale are written after the reuse decision).

## Verified

- he_tests rebuilt; `he_tests --source-file=*test_terrain_tools_ui.cpp`: 14/14 green.
- Negative control: all 11 new conditions removed -> exactly the 11 new subcases fail
  (pristine + 4 step-10 subcases still pass); conditions restored afterwards.
- `scripts/editor_help_audit.py --check`: ok (no new controls).
- Full he_tests (scratch HOME/HE_CONFIG_DIR): 5977/5978 cases; the one failure is
  test_shortcuts_ui (persisted() empty), an artefact of the scratch HOME; the file
  alone with the normal environment passes 2/2. Nothing in it touches this change.

## Not verified / next

- CI on the pushed commit is watched by the Hive, not by this worker.
- Next: nothing open for this topic from this note.

## Handoff
Start from `TerrainTools.cpp:325`. If `FoliageLayer` gains a new hand-tunable field,
add it to this chain and a SUBCASE to the test of the same name.
