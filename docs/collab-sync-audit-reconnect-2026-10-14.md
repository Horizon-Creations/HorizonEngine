# Collab sync audit, step 7: every client gets every change (2026-10-14)

Topic 198, step 7. Question: does every participant receive every change, also after a late
join or a reconnect? Answer before this step: **only while connected, and not even then for a
late joiner's own edits.** This doc lists what was checked, the three gaps found, and what
closes them.

## 1. The live link: no loss, no reordering (checked, unchanged)

* Transport is TCP, direct (`TcpTransport`) or through the relay (`RelayTransport`, TCP to the
  relay), wrapped in `SecureTransport`. Ordered and reliable; nothing in `NetSession`,
  `RelayFanout` or `SecureTransport` drops a frame on a live, authenticated link.
* The host is the single relay point and re-checks the lock on every relayed transform, delta,
  component blob and asset frame (`CollabSession::handleDocDeltas`, `handleTransformUpdate`,
  …). A lock changes hands on the same ordered link, so the host's relay order **is** the
  order of edits between holders.
* So per-frame sequence numbers or acks on the live channels would add nothing. **Decision:
  none added.** The guarantee rests on the transport being reliable. Trap for a future UDP
  path: transforms are sent `SendMode::Unreliable` (`CollabSession::sendTransform`); harmless
  on TCP, lost final values on a lossy transport.
* A dropped link ends the session for that client (`CollabController::update`: "The connection
  to the host was lost", status Failed). Recovery is a rejoin, which is where the gaps were.

## 2. Gaps found and closed

| # | Gap | Effect before | Fix |
|---|---|---|---|
| 1 | Receive filter `m_docRevision` was **one counter per document**, shared with the own publish counter, never cleared | A late joiner (or an editor restarted and rejoined) numbers its first edit 1; every peer that had seen e.g. 3 for that document dropped all its deltas and saves **silently** until its counter passed theirs. Also across sessions, ids are reused. | Receive filter keyed by **(originator, document)** (`m_seenRevision`, `revisionKey`); own counter separate (`m_docRevision`); rows of a leaving participant dropped (`forgetRevisionsFrom`); both cleared in `teardown()`. Same-holder ordering (the v9 reason: a whole file behind that holder's newer deltas) is kept. |
| 2 | A joiner got the scene snapshot and nothing else | Files saved or created in the session, deletes/renames/folder creates, and the GameInstance graph (deltas only) never reached a late joiner or a rejoiner. | Host keeps `m_sessionTouched` (keys saved, created, delta-edited) and `m_sessionOps` (applied Delete/Rename/Create-folder), and `catchUpJoiner` replays both to every joiner after its snapshot: ops in order, then the current disk bytes of each touched key (`CollabSession::sendAssetOpApplyTo`, `sendAssetTo`; revision 0). Editor applies a caught-up `::GameInstance::` file by reloading the graph and resetting its mirror (`EditorApplication.cpp`, `onRemoteAsset`). |
| 3 | `publishAsset` on a client without the lock asked for it and returned | One-shot writes (MCP save, rename sweep, background write) never left the machine. | `m_publishOnGrant`: the grant (`onLockChanged`) re-reads and sends the file, then gives the lock back unless a tab here holds it. A deny drops the entry. |
| 4 | No way back after "connection lost" short of retyping the join | — | **Rejoin** button in the Collab panel (`canRejoin`, `rejoinLastSession`; not after a ban, not for a host). |

## 3. What a joiner receives now

| State | Carried by |
|---|---|
| Scene, entities, components, Level Script, landscape grids | snapshot (unchanged) |
| Lock table, presence | sent after the snapshot (unchanged) |
| Files saved / created during the session | catch-up file, current host disk bytes |
| Deletes, renames, folder creates during the session | catch-up op replay (a delete/rename is skipped while its source exists again on the host) |
| GameInstance graph edited during the session | catch-up file `::GameInstance::` (host writes it on every applied delta) |
| Edits the holder made in the last ~1 s that are only in deltas | the holder's next autosave (same baseline as before) |

## 4. Decisions

* State-based catch-up (host disk) instead of an op log with per-client sequence numbers: the
  host already applies everything, so its disk is the session's state; no wire change, no
  protocol bump (same frame ids, no new shapes).
* Only files the **session** touched are sent. Differences that predate the session are source
  control's (project key check on join). A joiner's untouched files are never overwritten.
* Host-authoritative on rejoin: whatever a client changed while cut off is replaced by the
  snapshot and the catch-up. No merge. Said in the Rejoin help text.

## 5. Known limits

* Rename A to B followed by a new A: the rename is skipped in the replay (A exists on the host);
  the joiner keeps its old A (overwritten if the new A was touched) and gets B only if B was
  saved in the session.
* Catch-up obeys the transfer ceiling (both sides); an oversized file is skipped with a log line
  on the host and a refusal notice on the joiner, as for live saves.
* Asset kinds that do not travel (meshes, textures without large-asset sync) are not caught up;
  they were never in the touched set.

## Handoff

- **Topic / step:** 198, step 7 of 11
- **Branch / commit:** `claude/audit-vollst-ndige-bestandsaufnahme-was-ber-live-collab-sitz` @ 4de4c611 + this doc commit (pushed)
- **Done:** a late joiner's and a rejoiner's edits now reach every peer; every joiner is caught
  up on the session's files, ops and GameInstance graph; a client's save that waited for the lock
  goes out on the grant; Collab panel offers Rejoin after a lost connection.
- **Files touched:** `src/HE_Editor/CollabController.cpp:2932` acceptRevision (per originator),
  `:4405` noteSessionOp, `:4435` catchUpJoiner, `:1058` join/leave hooks, `:1468` publish on
  grant, `:3090` deferral, `:2147` canRejoin/rejoinLastSession/redialLastJoin;
  `CollabController.h` (m_seenRevision, m_sessionTouched/Ops, m_publishOnGrant, LastJoin.banned/joined);
  `src/HE_Net/src/CollabSession.cpp:2020` sendAssetTo / sendAssetOpApplyTo;
  `src/HE_Editor/EditorApplication.cpp:939` GameInstance file apply; `CollabPanel.cpp:1360` Rejoin;
  `EditorHelp.cpp` "Collaboration Session/Rejoin".
- **Verified:** out/build/release: `he_tests` (75 steps) and `HorizonEditor` (85 steps) build rc 0.
  New tests `test_collab_controller.cpp:3517-3745` (5 cases, 52 assertions) green; negative
  control: with the path-only revision key and without the grant deferral, "Revisions" and
  "Lock grant" fail. test_collab_controller 86/86; test_net_collab 113, collab_docsync 24,
  collab_undo 9, editor_help 19, mcp_tools_collab 7, terrain_sync 10, editor_commands 13. Full
  he_tests minus test_material_graph (`HOME=$(mktemp -d)`): 5132/5132. `editor_help_audit.py
  --check` 1168/1168.
- **Not verified:** no live two-editor run (Rejoin button, GameInstance catch-up in
  EditorApplication.cpp: build-checked only, not in he_tests). The kick stands in for a dropped
  link in the rejoin test. Relay path not exercised (same frames, TCP to the relay).
- **Decisions taken:** no seq/acks on live frames (TCP + host relay order); state-based catch-up
  from the host disk, only session-touched files; no protocol bump (no new frames or shapes);
  rejoin is host-authoritative, offline edits are replaced.
- **Decisions suggested, not taken:** switch `sendTransform` to reliable before any UDP path;
  a joiner notice "caught up on N changes" (only a host log line now).
- **Pending:** step 8 (save/idle hook to the server) can reuse `m_sessionTouched` as "what the
  session changed". Rename-then-recreate replay limit (section 5).
- **Traps:** CollabSession.h edit rebuilt only 75 he_tests steps here, not ~410. `cp` back of a
  backed-up .cpp updates mtime, so ninja rebuilds it (good for a negative control).
  `onParticipantJoined/Left` had no caller before; the controller now owns both slots.
- **Next steps:** 1. Step 8: hook `EditorUI::saveAsset` / idle detection, start from
  `CollabController::m_sessionTouched`. 2. Step 11 live run: drop Bob's network (not a kick),
  edit a material + GameInstance on Anna, press Rejoin, check both arrive; late join with Cara.
