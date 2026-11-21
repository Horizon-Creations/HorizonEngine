# RVT 2.0 step 8: per-layer stale flags (Thema 335)

The pool has three layers per page (0 colour, 1 normal, 2 AO + roughness). Until now a page had one `dirty` bool, so a
changed Normal/Roughness/AO input repainted all three layers of every resident page. Now the flag is a 3-bit mask, and a
repaint paints only the stale layers.

## What changed

* `rvt::RvtManager` (`RuntimeVirtualTexture.h/.cpp`): `Page::dirty` is a layer mask. `invalidateRect/invalidateVolume(.., layers)`
  (default all three, so old callers and witnesses behave as before) OR the mask in; `PageRender::layers` carries the stale
  layers of a repaint (all three on a first paint); `dirtyLayers(key)`, `Stats::dirtyLayer[3]`. `dirtyVersion()` only moves when a
  layer turns stale that was not stale yet; the debug view's stale mark means "any layer".
* `HlodBaker::renderRvtPage(.., layers)` and `rvt::paintPages` skip layers that are not named (`LandscapeHlodBaker.cpp:547`).
* `matRvtLayerState(split, state, rvtName)` (`RvtSplit.h/.cpp`): per layer a hash of the page hashes it is cut from and the union
  of what those pages read of the ground. Same pin-to-layer cut as `matRvtLayerViews` (layer 2 = Roughness + AO pins).
* `RvtLandscape` (`RvtLandscape.cpp`): `Segment::layerHash[3]` is what the resident pages were last marked/painted at. A page
  rebuild (parameter before the Write node) marks only the layers whose hash moved (`markStale`); a change that sits in no painted
  layer (optional Write pin) marks nothing. Terrain journals mark per layer too: each layer asks the journal for the edit kinds its
  own pages read (`Analysis::layer.reads[k]`), so a sculpt stroke skips a layer that reads only the weights. A material without
  RVT nodes is one program: all layers, as before. `FrameStats::layersStale[3]` counts pages marked per layer.

## Verified

* `ctest -R "test_rvt|test_hlod|test_landscape|test_terrain"` on Metal (macOS, Release): 23/23 green. New cases:
  `test_rvt_core` "a page is stale per layer, and a repaint names exactly the stale layers";
  `test_rvt_page_invalidation` "a changed Write pin moves the hash of its pool layer only" and "a layer's reads are those of the pages
  it is cut from"; `test_rvt_auto_landscape` "a changed parameter marks exactly the pool layers whose Write pins it reaches"
  (real Auto Landscape graphs, five variants).
* Witness `HE_DUMP_RVTDRIVER=1 HE_DUMP_AUTOLAND=1 HE_RVT=1 HE_DUMP_RHI=Metal HE_HIDDEN_WINDOW=1 HE_DUMP_PATH=.. HE_DUMP_QUIT=1`
  (private HOME), full output in `rvt-2-0-step8-layer-stale-witness-2026-11-09.txt`. New step G multiplies a "Witness Rough"
  float into the graph in front of the Roughness pin only and then moves it: **re-attached NO, 1 page rebuild, 21 pages marked
  stale, per layer colour/normal/AO+rough marked 0/0/21**, 17 repainted. The old steps still pass (snow line: 0 stale; sculpt 7 /
  paint 6 pages; rock slope: 21/21/21, since it feeds the layer blend that all three layers read).
* Every shipped Auto Landscape parameter that a page reads moves all three layers (they all feed the shared layer blend), so the
  shipped material gains nothing yet from the per-layer flags; the Witness parameter (and test) is what shows the isolation. A
  material whose Normal/Roughness/AO inputs are fed by their own parameters gets the saving.

## Not verified

* Windows backends (D3D11/D3D12/Vulkan/GL): no backend-specific change, not run. Per-layer painting draws the same single view with
  the same `rvtLayer` as before, only fewer of them per page.
* GPU cost of a one-layer repaint against a three-layer one (not measured; out of scope).

## Handoff

* **Topic / step:** Thema 335, step 8 (step 9 of the board).
* **Done:** see "What changed"; the Done-when items are met (witness 0/0/21, tests green, branch pushed, CI started).
* **Decisions taken:** the mask is keyed by pool layer (3), not by Write pin; an unconnected or optional pin is in no layer. A page
  change that moves no painted layer marks nothing. Journals mark per layer by what each layer's pages read.
* **Pending / traps:** the witness's pages that are not in view (4 of 21 at 110 m above, near radius 160 m) stay stale in every
  layer after a change, as before (`dirty` never reaches 0 in that step): not a regression, listed as "stale at the start 4/4/4".
  `Page::dirty` is now a mask: do not assign `true`/`false`. A new `Stats`/`PageRender` aggregate init must not rely on field order.
* **Next steps:** (1) measure a one-layer repaint on a big view (`he_perf_capture.py`); (2) a Windows run of the new witness step
  (no backend change expected); (3) optional: let `Stats::dirty` ignore pages outside the camera's wanted set.
