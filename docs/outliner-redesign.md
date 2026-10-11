# World Outliner redesign (2026-10-09)

The Outliner showed a name per row and everything else as text in brackets. You could not see
what an entity *was* without selecting it, the eye and the padlock sat on every row, the
type filter was a 20-entry dropdown that listed kinds the scene has none of, and a deep
hierarchy had no guide lines.

## Rows
* **Type icon**: a coloured square with a letter (M mesh, L light, C camera, ...). One
  icon per entity, by a fixed priority (`OutlinerFilter::primaryKind`): a light on a mesh
  is a light first. An entity with none of the filterable components is a group (hollow
  square). The glyphs and colours are `OutlinerFilter::kindGlyph/kindColor`.
* **Eye and padlock** show while the pointer is on the row. A hidden entity keeps its
  slashed eye in the accent and its row dimmed (50 %); a locked one keeps its padlock.
  They are still items of their own (`ItemAdd` + `ButtonBehavior`), so a click on one
  is not a click on the row.
* **Badges** are pills: the prefab asset name (filled when the placement was changed
  here), `+` for something added to a placement, the collaborator holding a lock
  (`you` when it is yours). They give way to the name last-but-stub: the name keeps at
  least 56 px, then the last badge, then the child count are dropped.
* **Folded rows** show how many rows are inside: `Props (3)`.
* **Selection** gets an accent bar at the left edge; the indent guide lines of every
  ancestor of the selection are drawn in the accent.
* **Search hits** are marked in the name (`OutlinerFilter::matchSpan`).
* **Row height** is one value (`rowHeight()` = 1.6 x font size) for every row, root
  included: the row clipping measures it. The row is a label-less `TreeNodeEx` with
  `FramePadding` set so it is that tall; everything else is drawn over it.

## Header
* Search box, `+`, and a clear button as before; the type dropdown is gone.
* **Type chips** with counts, only for kinds the scene contains (and the active one even
  at zero). Click = filter to that kind, click again = all. At most three lines; the
  rest are behind a `+N` chip. The counts are swept a slice of the cache per frame
  (`OutlinerFilter::tally`, 2048 entities a frame) and published when a sweep ends.
* **Status line**: `N entities` (`M of N` while filtered) and, on the right, the path of
  the selection, or `N selected`.

## Interaction
* **Rename in place**: double-click on a name, F2 on the selection, or the row menu's
  *Rename*. Enter or a click elsewhere keeps the name, Esc drops it. The old modal is gone.
* **Drop on a row's edge**: top quarter = before, bottom quarter = after (under the same
  parent), middle = child (the bottom edge of an *open* branch also means child).
  `HorizonWorld::placeNextTo` does the move. A line (or an outline for "child") shows
  where it will land; illegal drops (onto itself or its own subtree) show nothing.
* **Alt-click on a folded row's arrow** opens everything under it.

## Tests
`tests/test_outliner_ui.cpp` (chips, hover/hidden/locked icons, F2 and double-click rename,
drops, Alt-click, a showcase frame for `HE_UI_DUMP_DIR`), `tests/test_outliner_filter.cpp`
(`primaryKind`, `tally`, `matchSpan`), `tests/test_outliner_row_actions.cpp`
(`placeNextTo`).
