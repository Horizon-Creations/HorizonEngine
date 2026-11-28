# Node graph: node layout redesign (2026-11-25)

All node graphs (HorizonCode class/level/widget graphs, Material, Particle, Animator
state machine) draw through `GraphEditor::draw`, so the redesign is in one place.

What changed, in graph units:
- **Width fits the content** (`measureNode`, `src/HE_Editor/GraphEditor.cpp`): title,
  input column (pin, label, inline editor), 16 px gap, output column, rounded **up**
  to 16 (half the 32 grid). It used to be 176 for every framed node. Nodes with an
  on-node body stay at least `kNodeW` (176), because host body widgets are sized
  against it. Minimum 80 framed, 48 compact.
- **Room for inline editors does not depend on wiring.** Before, wiring a pin hid its
  editor and the node shrank, which moved its output pins under the wire.
- **Pins sit inside the box**, `kPinInset` = 10 from the edge, label 8 past the pin.
- **Height is unchanged** (title 24 + rows × 20 + body + 6), so saved graphs and
  comment boxes keep their arrangement. Shrinking width only adds gaps.
- **Scaling**: whole-pixel node position, size, pin rows and text. Corner radii and body
  insets scale with zoom. Titles are centred in the bar. Inline editor frames are
  centred on the pin row (they sat 1–2 px high). Below zoom 0.46, pin labels and inline
  editors are left out, because the labels would be under 6 px.
- **Body widgets were scaled twice** (zoom²). `draw` pushes a zoomed style for the body,
  and Material/Particle/Animator called `pushWidgetScale` again inside it. A nesting
  count (`widgetScaleNesting`) now makes the inner call a no-op.
- Public layout: `GraphEditor::nodeLayout(model, id)` (size, editor column) and
  `pinGraphOffset(model, id, pin, input)`. The material preview halo and the tests use
  them instead of re-deriving the geometry.
- HorizonCode "Self" placeholder sits on the label baseline (`HcEditorUtil.cpp`).

Second round (same day, Blueprint as the model):
- **Hit band per pin**: the dot and its label, from just outside the node's edge through
  the end of the label, never into the inline-editor column (`Drawn::hitX0/hitX1`,
  `pinAt`). A node in front hides the bands of nodes behind it. The circle test stays
  as the fallback for low zoom. A faint plate and a white label show the row under the
  cursor.
- **Pure node shapes** (`Shape` in `GraphEditor.cpp`):
  - *Operator*: `Model::compactTitle` returns a symbol. There is no title strip and no
    A/B labels, and the symbol is drawn large in the middle. The HorizonCode host maps
    Add/Subtract/Multiply/Divide/Greater/Less/Equals/And/Or/Not.
  - *Pill*: no inputs, one output whose label ends the title ("Get Self" → "Self"). Just
    that row.
  - *Compact*: any other pure node keeps its slim title strip.
  - Compact shapes have 4 px padding and snap to 8 rather than 16.
- **Hollow until wired**: exec triangles and scalar dots are outlines while no wire is on
  them. Container glyphs stay filled.
- Round 3:
  - An operator's output sits level with the symbol, centred on its inputs
    (`NodeMetrics::outRowShift`).
  - Word operators (AND/OR/NOT) are drawn at 14 px rather than 20.
  - `pinInlineEditorWidth` may now also ask for less than the 58 px default. HorizonCode
    Bool pins ask for 16, a checkbox, which makes Branch, NOT and every bool parameter
    narrower.

## Handoff

- **Branch / commit:** release/0.8.0, cfbbfd2b7 (layout), then "Node graph: a pin's
  label is part of the pin; operator chips…" (round 2).
- **Round 2 verified:** `test_graph_editor_keys` 25/25 (new: a label press takes the pin,
  the node's middle does not), shots looked at, full ctest 384/384, help audit.
  Known: at zoom ≤ 0.6 the headless raster draws the hollow ring as a small square; the
  GPU backends anti-alias lines, which was not looked at.
- **Files:** `GraphEditor.{h,cpp}` (`measureNode`, `nodeLayout`, `pinGraphOffset`, draw
  pass), `MaterialEditorPanel.cpp` (halo uses `nodeLayout`), `HcEditorUtil.cpp`
  (Self baseline), `tests/test_graph_editor_keys.cpp` (pin helpers ask the canvas; 2
  new layout cases), `tests/test_ui_shot.cpp` (new shot "a HorizonCode graph's nodes,
  at three zooms"; dropdown case asks `nodeLayout` for the editor column).
- **Verified:**
  - Headless shots `node-graph-z{060,100,150}` (`HE_UI_DUMP_DIR`) before and after,
    looked at.
  - Material editor dump looked at: unchanged at zoom 1.
  - `test_graph_editor_keys` 24/24, `test_ui_shot` 48/48, `test_editor_rewards`,
    `test_material_editor_ui`, `test_hc_watch_ui` green; full ctest and help audit
    (see commit).
- **Not verified:**
  - The running editor window. In particular: dragging wires onto inset pins by hand,
    and Particle/Animator bodies at zoom ≠ 1 (the double-scale fix changes how those
    look there).
  - Windows CI.
- **Decisions:** height kept for saved-graph compatibility; pins inset (Unreal-style)
  rather than on the edge; LOD threshold 0.46.
- **Next steps:**
  1. Look at Particle and Animator graphs at 0.5 / 1.5 zoom in the editor.
  2. Optionally hand body widgets `bodyMax.x - bodyMin.x` instead of `kNodeW - N`, so
     body nodes can shrink too.
