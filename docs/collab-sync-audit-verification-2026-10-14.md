# Collab sync audit, step 11: verification with several editors (2026-10-14)

Topic 198, step 11. Task: several clients, a full pass over every object type a live session
syncs, build and tests, report. Steps 1-10 each left a "Step 11:" item in their handoff; the table
in section 2 answers them one by one.

## 1. How it was run

* `scripts/he_collab_full_pass.py OUTDIR [--team-server]` (new): three deployed Release editors,
  hidden, each with its own HOME and project copy, driven through their own MCP bridges
  (`HE_MCP_COLLAB_CONTROL=1`, `HE_COLLAB_OFFLINE=1`, real loopback TCP). A hosts, B joins at
  once, C joins late. Every change is made on one editor and read back on another. About 2 min.
* `scripts/he_collab_two_editors.py` (topic 86) as the baseline for session/lock behaviour.
* `--team-server` writes `"sourceControl": {"mode": "teamServer"}` into the project copy
  before the editors start (step 9's opt-in).

## 2. Results against the open items

| Item (from the step's handoff) | Result | How |
|---|---|---|
| Entities: create, move, **scale**, property, rename, reparent, delete (steps 1/2) | **live, green** | A creates 3, B moves+scales [2,3,4], B sets `light.intensity` + name, B reparents (world position kept), B deletes; each read on the other editor |
| Group, Remove Component (step 2) | test only | no MCP path; `test_editor_commands`, `test_collab_controller` |
| Delete / rename of a peer-held entity (steps 2/6) | **live: refused** | A's delete, rename and reparent of B-held entities come back `locked_by_other`, B still has them unchanged. The host in-tray *ask* is UI-only (no MCP approve tool): not live-verified |
| Landscape: two editors sculpt, compare byte for byte (step 3) | **live, green** | A sculpts (0,0), B sculpts (-16,16), each reads the other's height; all three editors save and the three `sculptHeightsB64` are byte-identical (16900 B). Different spots, not the same spot at once |
| HorizonCode, each HC host (step 4) | **live, green after fix 2** | Level Script: A adds variable, node, moves the node, B reads. Game Instance: B adds variable + node, A reads. Class assets and the animator sync graph: not live (MCP edits a class only with its tab open, and no tool opens one) |
| Materials (step 5) | **live, green after fix 1** | B (client) creates a master, sets Roughness, A reads; A sets Metallic, B reads. Material functions and instances: not in the run (test only) |
| Late join (step 7) | **live, green** | C joins after everything: same content uuids, scale, intensity, parentage, both sculpted heights, both HC graphs, the moved node, the material value |
| Rejoin, edits while away (step 7) | **live, green** | B leaves; A moves an entity, sculpts, adds a Game Instance variable, sets a material param; B rejoins with the same code and has all four, C (stayed in) saw them live. A clean leave, **not** a dropped link |
| Save + idle (step 8) | **live, green** | B's `scene_save` → A writes its own scene file within 4.5 s, log `session state kept: ::Scene:: (saved)`; then C's edit and 5 s of quiet → it is in A's file (~6 s), log `(idle)` |
| Team server opt-in (step 9) | **partly** | whole pass 77/77 with the opt-in in place. Not visible headless: the panel's "waiting for the team server" line, and whether `GitController` took team mode (it logs nothing) |
| Contributor list for the hand-over (step 10, 3.3) | not done | code change in the hand-over path, kept out of a verification step |

Totals: full pass **77/77** (git mode) and **77/77** (`--team-server`), two-editor baseline
**42/42**, after the fixes below. Before them the first complete run had 16 red: 10 HC (one of
them the script's `hc_set_node` payload), 2 for the material value, and 4 that were the script's
(3 TerrainChunk uuid comparisons, 1 log timing).

## 3. Found and fixed (commit 51442c38)

1. **A client's lock release took effect only at the host's echo** (lost update).
   `CollabController` hands a claim straight back after its grant-send
   (`CollabController.cpp` ~1504). Until the host's echo the client's table still said "mine",
   so a save in that round trip went out, the host refused it (`CollabSession.cpp:1767`, "does not
   hold lock"), nobody told the client, and the host's next save overwrote the client's value.
   Hit by MCP `material_create` + `material_set_param` on a client; any one-shot write right after
   a grant-send can hit it. Fix: `CollabSession::releaseLock` (client branch) drops the lock locally
   at once; the next save asks again and goes with the grant (`m_publishOnGrant`).
2. **MCP `hc_` edits to the Level Script / Game Instance with the tab closed were never sent.**
   `updateAssetCollabSync` walks `m_tabs` only (`EditorApplication.cpp` ~9762); the comment in
   `McpToolsHc.cpp` promised otherwise. A human always edits with the tab open, so step 4's fix
   did not cover it. Fix: `hc.beginEdit` seeds the mirror before the mutation, `hc.endEdit` marks
   the graph (`m_collabUntabbedEdits`), the collab pass asks the host, claims, sends the diff and
   lets the lock go once nothing differs. Nothing runs per frame without such an edit.

Tests: `test_net_collab` "a client's release is final at once, not one round trip later",
`test_collab_controller` "Lock grant: the next save right after the claim went back still reaches
the host". Negative control: with the old `CollabSession.cpp` both fail (3 checks / "fern-v2"
never arrives), with the fix both pass. Fix 2 is covered by the live run only (the MCP HC tools
run against a graph on the stack in `test_mcp_hc`, without `EditorApplication`).

## 4. Seen on the way (not changed)

* The MCP `terrain_*` tools take the **entity lock** (they write through `EditorCommands`,
  `McpToolsTerrain.cpp` ~205); the human brush does not (TerrainSync). Two MCP clients cannot
  sculpt one landscape at the same time; two humans can.
* Each editor builds its landscape's `TerrainChunk` children itself, so their uuids differ per
  editor by design (like Sun/Moon). The script compares them by name.
* A hidden editor's log reaches its file through buffered stdout, seconds late and with lines
  interleaved. Mid-run checks must watch files, not count log lines (the script does).

## Handoff

- **Topic / step:** 198, step 11 of 11
- **Branch / commit:** `claude/audit-vollst-ndige-bestandsaufnahme-was-ber-live-collab-sitz` @ 51442c38 + this doc (pushed: see the post)
- **Done:** every synced object type verified live with three editors incl. late join, rejoin, save/idle, team-server opt-in; two lost-update paths found and fixed.
- **Files touched:** `src/HE_Net/src/CollabSession.cpp` (releaseLock), `src/HE_Editor/EditorApplication.cpp` (hc.beginEdit/endEdit, updateAssetCollabSync untabbed loop), `EditorApplication.h` (`m_collabUntabbedEdits`), `McpToolsHc.cpp` (comment), `tests/test_net_collab.cpp`, `tests/test_collab_controller.cpp`, `scripts/he_collab_full_pass.py`.
- **Verified:** build `he_tests` + `HorizonEditor` (Release, `out/build/release`); full pass 77/77 both modes; two-editor 42/42; `test_net_collab` 114/114, `test_collab_controller` 88/88, `test_mcp_hc` 20/20, `test_mcp_tools_collab` 7/7; negative control above; full ctest: see the result post.
- **Not verified:** dropped link (only clean leave), relay path, host in-tray ask, Group/Remove Component live, HC class assets + animator graph live, material functions/instances live, panel text in team mode, Windows/Linux (CI).
- **Decisions taken:** fix only what the run proved lost and what was local (2 fixes); the contributor list (3.3) stays a follow-up; the release is local-first (the host's word still decides, a release of a lock we do not own changes nothing locally).
- **Traps:** run `he_tests` from `out/build/release`; the CI Windows red `test_collab_controller.cpp` catch-up path check (step 7/8 handoffs) is not touched here.
- **Next steps:** 1. Contributor list in `onSessionSettled` → `GitController::handOverToTeamServer` (spec 3.3). 2. A dropped-link rejoin in the script (kill B's socket, not `collab_leave`). 3. Topic 199 step 4 takes the gates in spec 3.11.
