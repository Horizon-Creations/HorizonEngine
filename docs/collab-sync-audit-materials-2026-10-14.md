# Collab sync audit, step 5: materials (2026-10-14)

Topic 198 step 5. Inventory read from the code (not run live), and the fixes for the gaps it
found. No new wire kind, no protocol bump: every change is in the editor.

## 1. How a material travels

Same two channels as HorizonCode graphs (docs/collab-sync-audit-horizoncode-2026-10-14.md §1):
item **deltas** while this editor holds the asset lock, and the 1 s **whole-file autosave**
(`ContentManager::saveAsset` hook → `CollabController::publishAsset`). `Material` and
`MaterialFunction` are in `HE::isCollabSyncableAssetType` (Types/Enums.h ~135); `Texture` is not
(only with "sync large assets").

| What | Edited in | Carried by | Before this step |
|---|---|---|---|
| Graph nodes (create / move / delete / values / params) | Material Editor canvas, side panel | `MatAdapter` `Kind::Node` | yes |
| Links | canvas | `Kind::Link` | yes |
| Comment boxes | canvas | `Kind::Comment` (`bumpNextId`) | yes |
| Material function graph + interface (FnInput/FnOutput) | same panel, function tab | same adapter | yes |
| Derived fields (`customShaderFragGlsl`, GBuffer, vertex body, param block, texture list, blend mode, domain) | regenerated from the graph | whole file only; **receiver never regenerated** | partly (see §2.2) |
| Instance overrides, static switches | instance tab override panel | whole file only (no graph) | yes, but see §2.1 |
| MCP material tools | `McpToolsMaterial.cpp` | `saveAsset` → whole file; refused while `lockedByOther` (~280) | yes |
| Material slot on a mesh / terrain material | Details / Landscape | entity blob / TerrainSync settings (steps 2, 3) | yes |
| Create / rename / delete a material asset | Content Browser | host-arbitrated asset ops | yes (conflicts: step 6) |

`MaterialGraph` holds only `version`, `nextId`, `nodes`, `links`, `comments`
(`materialGraphToJson`), so the adapter covers the whole graph.

## 2. Gaps found

1. **A peer's whole file never replaced an already-loaded asset.** `applyAssetBytes`
   (EditorApplication.cpp) wrote the file and called `loadAsset`, which is a lookup for a loaded
   path (ContentManager.cpp:1068). The tab reload that follows (`reloadAssetTabFromDisk` →
   `stateFor` → `openPanelAsset` → `loadAsset`) then re-read the **stale in-memory copy**, reverted
   the canvas to it and seeded its collab mirror from it. The 1.5 s hot-reload poll fixed the
   ContentManager a moment later but never reloaded the tab again, so an open tab could keep an
   old graph for good. If that editor took the lock later, its first save wrote the old graph out
   for everyone. **Cross-cutting:** HorizonCode classes, widgets, particle and animator tabs read
   through the same `loadAsset` path (step 4 verified that path by reading only).
2. **The receiver never regenerated.** A delta batch patched `State::graph`, and `afterApply` only
   set `previewDirty`. The live `MaterialAsset` (what the scene and the preview draw) stayed on
   the old shader until the file arrived. The same applied to instances of that master and to the
   function-graph cache (`s_fnGraphCache`, cleared only by the panel's own edit).
3. **Undo reached past a peer's edit.** The panel's undo stack (whole-graph snapshots) survived
   remote batches and reloads. Ctrl+Z restored a pre-peer snapshot, and the first diff after this
   editor took the lock sent it out.
4. **A master reloaded from disk left its instances stale.** The poll only called
   `InvalidateMaterial`, and `syncMaterialInstancesOf` ran only inside the panel's own edit. A
   function file reload left `s_fnGraphCache` stale.
5. Instance tab: `stateFor` appended `parentSwitches` on every reload (doubled rows); the tab bound
   an always-empty graph to the delta channel.

## 3. Implementation

* `EditorApplication::applyAssetBytes`: a loaded path is unloaded and loaded again (same UUID,
  what `pollHotReload` does), the id is queued in `m_reloadedByPeer` and the poll is forced for the
  next frame. The poll merges the queue into its `changed` list, so a peer's file gets the same
  per-type follow-up as a hot reload (renderer caches, prefab placements, scripts in play).
* Poll follow-up: `Material` masters → `syncMaterialInstancesOf(path)`; `MaterialFunction` →
  `MaterialEditorPanel::forgetFunctionGraph(path)` (new). This also helps git pulls.
* `MaterialEditorPanel.cpp`: `applyToMaterial` split into `regenerateMaterial` (no dirty mark) and
  the dirtying wrapper. The delta callback sets `State::peerEdited`; the next `render()`
  regenerates the live asset, re-derives its instances and resets the undo floor. Reloads
  (`reloadFromDisk`, `reloadByContentPath`) drop the undo history and the function's cache entry.
  Instance tabs return no delta binding; `parentSwitches` is cleared before refill.
* Test: `test_contentmanager.cpp` "a peer's file over a loaded material needs unload + load" pins
  the `loadAsset` trap, the unload + load fix (same UUID) and the instance re-derive.

## 4. Not done here

* Instance overrides stay whole-file only (~1 s behind). A live channel would need a new kind
  plus a ContentManager hook in the adapter; not worth a protocol bump for a value panel.
* An inactive material tab regenerates on its next `render()` (or when the file lands); the
  scene follows the file in that case, about 1 s later.
* Lock denied (`onAssetLockDenied`): the loser's live asset already holds its forked shader
  (`applyToMaterial` writes it) and the disk is unchanged, so "reload from disk" re-reads the fork
  until the winner's first autosave lands. Step 6.
* MCP save on a **client** without the lock: `publishAsset` requests the lock and returns until
  the grant (CollabController.cpp ~3024). Nothing re-sends the save, so a one-shot MCP edit can stay
  local until the next save. Step 7 (lost updates).
* A texture the holder imports and wires in does not travel unless the session syncs large assets.
  The peer renders the material with that slot missing.
* A delta batch that arrives between a file landing and the tab's next `render()` is dropped
  (`collabDocs` answers empty while `loaded` is false); the holder's next autosave covers it.

## Handoff

**Done:** inventory (§1-2), fixes for §2.1-2.5 (§3). Files: EditorApplication.cpp
(`applyAssetBytes`, hot-reload poll), EditorApplication.h (`m_reloadedByPeer`),
MaterialEditorPanel.cpp/.h, tests/test_contentmanager.cpp.
**Verified:** see the step's result post (he_tests build + listed test files, HorizonEditor build).
**Not verified:** no live two-editor session. Neither `EditorApplication.cpp` nor
`MaterialEditorPanel.cpp` is in he_tests; the editor-side changes are build-checked only.
**Decisions:** no new `CollabDocSync::Kind`, no protocol bump; explicit unload + load on arrival
instead of waiting for the mtime poll (the hot-reload test has to push mtime 2 s forward for
same-second saves); undo floor reset on peer edits (the scene's "their edit is not on our stack"
rule).
**Next steps:**
1. Step 6: lock-denied fork (§4) for materials and every panel that writes its live asset; apply
   the undo-floor reset to the HorizonCode / particle / animator / widget panels (same shape).
2. Step 7: MCP client save without a grant (§4); dropped batches during a tab reload.
3. Step 11: live run: holder edits a master, a function and an instance; the peer watches the
   scene, an open tab and an inactive tab of each.
