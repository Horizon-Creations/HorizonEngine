# RVT 2.0 step 3: automatic repainting (Thema 335, 2026-11-08)

The engine derives what makes a page stale from the page subgraph (`src/HE_Core/.../RvtSplit.{h,cpp}`):

| Trigger | Mechanism |
|---|---|
| Param/switch/texture/function BEFORE Write | `matRvtPageStateHash(graph, page, env)`: page.hash of the LIVE nodes (a Static Switch follows only its taken input) + effective param values (material slots, entity `paramOverrides` on top), switch values, asset revisions. One hash per Write pin, folded by `matRvtMaterialState`. |
| Anything AFTER Read | not a page dependency, so never in a hash. Driver counts it in `FrameStats::liveOnlyChanges`; no re-attach, no page. |
| Sculpt / paint | `TerrainComponent::rvtJournal` now has `height` and `weights` parts (union fields kept). Driver takes `selected(kinds)` for the kinds `seg.reads` needs (`kRvtEditSculpt` = Height|Normal, `kRvtEditPaint` = Weights); unread edits are dropped (`FrameStats::journalIgnored`), the scratch copy is not even synced. |
| Entity overrides | part of the env values. |
| Tile load/unload | a tile is a landscape = a segment; `RvtLandscape` already attaches/removes volumes. Neighbour-edge effects are not modelled (LandscapeStore of PR 214 is not on this branch). |

Node reads: WorldPos -> Height; NormalWS and Normal Map nodes -> Normal+Height; Layer Blend / Group Pick -> Weights; Function Call -> everything unless `env.functionReads` says otherwise. Per-frame inputs (Time, Panner, Camera, Weather, ...) are listed in `MatRvtPageDeps::frozen`, frozen at paint time, never repaint.

Driver (`RvtLandscape.cpp`): `refreshAnalysis` (graph text hashed per tick, split only when it changes; asset revisions per contentEpoch) + `stateHash`. A material without RVT nodes keeps the Thema 333 behaviour (whole material = page input, all ground kinds read).

## Handoff
Verified: `he_tests` (release, this Mac) `*test_rvt_*` 41 cases green incl. new `test_rvt_page_invalidation.cpp` (param after Read = identical state, before Write = only the pin that reads it, switch pruning, texture revision, edit kinds) and journal kinds in `test_rvt_journal.cpp`; terrain/material_graph/landscape/hlod 318 cases green; HorizonEditor builds.
Not verified: the driver end to end (no material with RVT nodes exists until step 4, no fake renderer); "0 pages painted" is proven on the hash level only. Texture revision = shape + strided pixel sample, refreshed on `contentEpoch()` change only.
Next: step 4 must (a) feed `seg.analysis` with the real auto-landscape graph, (b) make the consumer material pick up live (after-Read) params without a re-attach, (c) paint per page graph so a changed pin repaints only that layer (today a page-state change re-attaches the whole segment and repaints all resident pages).
