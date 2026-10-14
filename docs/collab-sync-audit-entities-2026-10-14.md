# Collab sync audit, part 1: entities (2026-10-14)

Topic 198, step 1. What a live collab session replicates for **scene entities**
(create, move, scale, every property, hierarchy, delete), what it does not, and where.
Inventory only: nothing here is fixed yet; step 2 implements the gaps.

All statements below were verified **by reading the code** at the line given, on
`release/0.7.0` @ f18742ce. Rows marked *test* also have a test that exercises them.
Nothing was run in a live two-editor session for this step.

## 1. The four paths an entity change can take

| Path | Local trigger | Wire | Receiver | Authority |
|---|---|---|---|---|
| **Transform delta** | per frame, primary selection only (`EditorApplication.cpp:4609-4658`); MCP `SetTransform` (`EditorCommands.cpp:327`); CollabUndo (`EditorApplication.cpp:584-604`) | `kMsgTransformUpdate/Relay` (`CollabSession.cpp:2276-2328`), `SendMode::Unreliable` | `EditorApplication.cpp:824-840` writes `TransformComponent` | lock required, host re-checks |
| **Component blob** | per frame, primary selection only, FNV-hashed (`EditorApplication.cpp:4664-4673`, `CollabController.cpp:2487-2515`); MCP `SetComponents` (`EditorCommands.cpp:365`) | `kMsgComponentsUpdate/Relay` (`CollabSession.cpp:1212-1255`) | `applyEntityComponents` (`EditorApplication.cpp:548-559`) | lock required, host re-checks |
| **Structural create/destroy** | diff of the entity set every frame (`EditorApplication::syncStructuralChanges`, `:10635-10761`; rule in `StructuralSync.h`) | `kMsgStructuralUpdate/Relay` (`CollabSession.cpp:1469-1530`) | `instantiatePrefab(preserveIds)` / `destroyEntity` (`EditorApplication.cpp:520-568`) | create: none; destroy: refused if a peer holds the lock |
| **Snapshot** | host captures `saveToMemory` at each join (`CollabController.cpp:138-143`) | `kMsgSnapshot*` | `world->clear()` + `loadFromMemory`, then `onWorldReplaced` (`EditorApplication.cpp:843-866`) | host only |

Wire identity is the entity's UUID (`netIdFor`, `CollabController.cpp:2519-2557`), not the
handle. **Doc drift:** `docs/networking-layer-design.md` "Live scene deltas (N6)" still
describes handle keys and `participantId<<32|counter`; that section is outdated.

## 2. Per operation

| Operation | Status | Why / where | Test |
|---|---|---|---|
| Create entity / drop prefab / duplicate / paste | **synced** | entity-set diff, one create per new subtree | *test*: `test_collab_controller` "Structural sync: …" (3048-3108), `test_net_collab` 1250 |
| Move / rotate / scale, single entity | **synced** | transform delta while the lock is held | *test*: `test_net_collab` 894, 958 |
| Move / rotate / scale, **multi-selection (group gizmo)** | **missing** | `ViewportPanel.cpp:1499` hands every selection root to `manipulateGroup`; only the primary is locked and published. The other members move on our screen only. The inspector gets this right (`InspectorPanel.cpp:107-138`, `membersToSkip`), the gizmo does not. | none |
| Property edit (any component, name) on the primary | **synced** | whole-entity component blob, hash-gated | *test*: `test_collab_controller` 1592, 1618 |
| **Remove Component** | **missing** | `applyComponents` (`SceneSerializer.cpp:1144ff`) only `emplace_or_replace`s keys present in the blob; there is no removal marker. A removed component stays on every peer. | none |
| **Reparent (outliner drag, MCP Reparent)** | **missing** | `CollabController::publishReparent` (`:2637`) has **no caller**. `OutlinerPanel.cpp:1406/1852` and `EditorCommands::applyReparent` (`:284`) never publish, and the component blob carries no parent (`parent` is entity-level, `SceneSerializer.cpp:1059`). Wire + receiver exist and are tested. | wire only: `test_net_collab` 1332 |
| Delete entity (nobody holds it) | **synced** | diff publishes destroy, host frees the lock | *test*: `test_net_collab` 1278, 1311 |
| **Delete an entity a peer holds** | **lost silently** | `sendStructural` refuses (`CollabSession.cpp:1476-1479`), but `syncStructuralChanges` ignores the result and erases it from `m_structureKnown` anyway (`EditorApplication.cpp:10756-10760`). Gone here, alive everywhere else, no message. No host-ask exists for entities (that pattern is assets-only: `AssetOp Delete/Rename`). | none |
| **Edit an entity a peer holds** | **lost silently** | inspector/outliner only *warn* (`InspectorPanel.cpp:405-423`); the edit is applied locally and every publish is refused. | none |
| **Ctrl+Z / footer Undo in a session** | **broken (plausible, not run)** | only the Edit menu routes to `CollabUndo` in a session (`EditorUI.cpp:2143-2156`). The shortcut and footer buttons (`EditorUI.cpp:3306-3340`) call `m_undo`, whose `restore` does `world->clear()` + `loadFromMemory` (`EditorUndo.cpp:55-59`); `makeContext` gates only on play (`EditorApplication.cpp:11211-11229`), `activeTabUsesWorldUndo` (`EditorUI.cpp:1221`) has no session check. Every entity gets a new handle, so the next diff publishes a destroy for every known entity and a create for the whole world, and it reverts peers' concurrent edits. | none |
| Final value of a quick drag | **can be lost** | `publishTransform` rate-limits to 33 ms per subject (`CollabController.cpp:2464-2468`) and only runs while the subject is held. Deselecting within that window drops the last value for good. The same applies to two MCP `SetTransform` calls on one entity within 33 ms. | none |
| Edits to non-selected entities by tools (terrain sculpt, foliage paint, splines, water) | **not covered by these paths** | change detection only looks at the primary selection. Terrain and other tools belong to step 3. | none |
| Scene-level data: `levelScript`, `cells`, `streaming` (`SceneSerializer.cpp:1078-1090`) | snapshot only | the level script has its own doc-delta mirror (`m_levelScriptMirror`, step 4); the cell manifest and streaming head do not sync live. | n/a |
| Late joiner | **synced** | host snapshot at join; TCP ordering puts later relays after `SnapshotEnd`. Reconnect / missed updates: step 7. | *test*: `test_net_collab` join tests |

## 3. Traps (not gaps today)

* Transforms go out with `SendMode::Unreliable`. Harmless now, because TCP (`TcpTransport.cpp:323`)
  and the relay (`RelayTransport.cpp:589`) ignore the mode. Put collab over `UdpTransport`
  and authoritative edits start being dropped. Switch them to Reliable if that ever happens.
* The host never runs `onWorldReplaced`, so its `m_structureKnown` is only correct because
  the first diff runs before anyone has joined. A leftover set from an earlier session is
  stale. It is also why the snapshot, not the diff, carries the host's state to joiners.
* `onWorldReplaced` (`EditorApplication.cpp:855-858`) records local-only entities
  (environment lights) as known. The next diff publishes a destroy for each of them. Peers
  ignore it, because those UUIDs are minted fresh per peer: wire noise, not damage.
* `setOnAssetSaved` on a **scene** writes the file on peers but does not reload the open
  world, so the live entity state is never clobbered by a save. Keep it that way.

## 4. Gaps for step 2, by priority

1. Session undo: route Ctrl+Z / footer Undo to `CollabUndo` in a session, or withhold
   `m_undo` there (`EditorUI.cpp:3306-3340`, `makeContext`). Highest damage, small change.
2. Reparent: publish on reparent (detect in `syncStructuralChanges` by remembering each
   known entity's parent, the same diff-not-hook rule), via the existing `publishReparent`.
3. Group gizmo: either lock + publish every moved member, or drop members we do not hold
   from `movable` in a session, the way `membersToSkip` does for the inspector.
4. Remove Component: carry removals (e.g. a `__removed` key list, or a full-replace flag
   in `applyEntityComponents`). Needs a test that an old blob without it still applies.
5. Delete/edit of a peer-held entity: refuse locally up front (or ask the host, step 6),
   and never mark a refused destroy as sent.
6. Trailing transform: flush the last value on lock release / deselect (step 7 overlaps).

## 5. Step 2: what was done about each gap

No wire format changed. Every new behaviour is optional for an older peer.

| Gap | Fix | Where | Test |
|---|---|---|---|
| 4.1 session undo | Footer, Ctrl+Z, native menu and Edit menu share one door (`undoNow`/`canUndoNow`): `CollabUndo` in a session. `ctx.undo` has a session backstop. `CollabUndo::setStructuralHandlers` was **never called**, so create/destroy/reparent/components entries popped and did nothing. Now wired as plain world edits that the diff publishes. | `EditorUI.cpp:1239`, `EditorApplication.cpp:644` | none for the routing (no UI harness drives the footer); syntax check + reading |
| 4.2 reparent | Diff, not hooks: parent per known entity (`m_structureParent`), `reparentedSince`, published after the creates with the new local transform in the message blob (old peers ignore it). Peer-held → put back + notice; parent not announced yet → wait. Echo protection in `onRemoteReparent`. Wire ids get cached when a parent is first recorded (`publishDestroy` could not name a never-selected child of a mid-session subtree). | `StructuralSync.h:104`, `EditorApplication.cpp:574, 10832`, `CollabController.cpp:2677` | `test_collab_controller` "a move in the hierarchy…", "a re-parent reaches the peer…" |
| (found) Group | A new group's create carried the entities it adopted, so they were duplicated on peers. `DetachKnownDescendants` keeps them out of the blob; the reparent pass moves the originals. | `StructuralSync.h:129`, `EditorApplication.cpp:10761` | "a new group does not carry the entities it adopted" |
| 4.3 group gizmo | In a session the gizmo takes only the primary + members whose lock we hold (inspector rule). Multi-move in a session is therefore primary-only until there is a multi-lock model. | `ViewportPanel.cpp:1505` | none (panel) |
| 4.4 Remove Component | Receiver applies collab blobs with the new `SceneSerializer::replaceEntityComponents`. It removes known components the blob lacks, except a terrain's generated mesh. `applyEntityComponents` stays update-only (multi-edit and spline patches). Every blob on this wire was always a full `serializeEntityComponents`, so no marker was needed. MCP `SetComponents` publishes the full entity, not the patch, and records full after-state. | `SceneSerializer.cpp:2774, 3483`, `EditorCommands.cpp` applyComponents | "a whole-state component write…", `test_editor_commands` "A component write publishes the whole entity…" |
| 4.5 peer-held delete | Delete/Cut (`withoutPeerHeld`) and `EditorCommands` destroy walk the **subtree** (the host only checks the named root) and refuse with a notice. The diff no longer forgets a refused destroy: it retries for 60 frames, then logs. | `EditorApplication.cpp:11108`, `EditorCommands.cpp:205` | `test_editor_commands` "A destroy is refused while someone else holds an entity under it" |
| 4.5 peer-held edit | **Not done**: the inspector still only warns. Step 6 (host asks) owns it. | | |
| 4.6 trailing transform | The rate-limited value is kept as pending and flushed by `update()` after 33 ms, before `followSelection` releases the lock, and before another subject's send. | `CollabController.cpp:2515` | "the end of a quick drag is not lost…" |

## Handoff

- **Topic / step:** 198, step 1 of 11 (entity sync inventory)
- **Branch / commit:** `claude/audit-vollst-ndige-bestandsaufnahme-was-ber-live-collab-sitz`, this doc only
- **Done:** inventory of every entity operation over a collab session, with file:line, wire path, authority and test coverage (sections 1-2); prioritized gap list (section 4).
- **Files a reader must open:** `src/HE_Editor/EditorApplication.cpp` 520-604, 824-866, 4602-4676, 10635-10761 (use offsets, the file is huge); `src/HE_Editor/CollabController.cpp` 1107-1145, 2439-2647; `src/HE_Net/src/CollabSession.cpp` 1212-1255, 1469-1530, 2276-2328; `src/HE_Editor/StructuralSync.h`.
- **Verified:** by reading only (grep + line reads), including that `publishReparent` has no caller, `applyComponents` never removes, and `activeTabUsesWorldUndo` has no session check.
- **Not verified:** nothing was run. The Ctrl+Z mass destroy/create is inferred from `EditorUndo::restore` + the diff, not seen in a live session. A two-process test (`scripts/he_collab_two_editors.py`) would prove it.
- **Decisions taken:** none in code; step 1 is inventory only.
- **Decisions suggested, not taken:** reparent detection by diff (parent per known entity) rather than hooking the outliner; group gizmo restricted to held members in a session as the cheap first fix.
- **Pending:** steps 2-11 as planned; section 4 is step 2's worklist.
- **Traps:** section 3; `networking-layer-design.md` N6 is outdated about ids; `test_collab_controller` "Larger assets" flakes under load (run it alone).
- **Next steps:** 1. Gap 4.1 (session undo routing) with a test that Ctrl+Z in a session goes to `CollabUndo`. 2. Reparent diff in `syncStructuralChanges` + test in `test_collab_controller` beside "Structural sync: …". 3. Group gizmo in session. 4. Component removal + old-blob compatibility test. 5. Peer-held delete/edit refusal (coordinate with step 6).
