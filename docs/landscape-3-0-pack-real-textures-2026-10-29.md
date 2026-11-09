# Landscape 3.0: real-texture packing per group (Thema 233, step 8)

`landscape_tex_gen --pack`/`--loose` worked for Mountains only (see
`docs/landscape-3-0-texture-groups-umsetzung-2026-10-29.md` §5.3/Traps): a group's three arrays
were always the in-memory placeholder, retoned. This step extends `--pack`/`--loose` to Forest,
Grassland and Desert, so real textures can be installed once the human delivers them. **No real
source textures exist on this device** (checked this worktree and the main checkout's
`EditorDeps/EngineContent/Textures/Landscape/**`, any `Source/` subfolder,
`scripts/landscape-textures/stage_polyhaven.py` output, and `hive_requests`/`hive_search`) — that
half of the step (install real PNGs, regenerate, upload to the EngineContent server) is still
blocked on the human, not on missing code.

## What changed

* `landscape_tex_gen <dir> --pack <png-dir> --group Forest|Grassland|Desert [--size N]`: reads
  `<RoleLabel>_<Map>.png` from `<png-dir>` (`<RoleLabel>` = the group's `roleLabels` with the space
  removed, e.g. Forest's "Forest Floor" → `ForestFloor_Albedo.png`; same Albedo/Normal/Roughness/
  AO/Height aliases `--pack` already accepted for Mountains) and builds the group's three arrays
  **straight in memory** — a group has no per-role `.hasset` files, its id block is only the
  material + 3 arrays (`AutoLandscapeMaterial.h:154-160`), so there is nowhere to put 15 more ids.
  A role with no files keeps the existing retoned placeholder look. The 5th slice (WetGround) is
  never packed for any group, Mountains included by this mode: it is not read by the material and
  every group keeps the shared placeholder there.
* `landscape_tex_gen <dir> --loose <png-dir> --group Forest|Grassland|Desert [--size N]`: splits a
  group's Roughness/AO/Height into their own files, `T_Landscape_<Group>_<Role>_<Map>.hasset`. New
  id block `kLooseGroupIndexBase` (offset `0xA8` from `kTexBaseHi`, i.e. `0x4A8..0x4CB`): right
  after Desert's `0x4A0..0x4A3`, leaving `0x4A4..0x4A7` free for a fifth group (per an existing
  Hive lesson on this file). The group prefix in the name matters: every non-Mountains group calls
  one of its roles "Snow", so an unprefixed name would collide between groups.
* Safety fixes that became load-bearing once `--pack` writes real data, not just placeholders:
  * `--arrays-only --group X` used to fall through to the placeholder generator (`writeGroupArrays`)
    — silently replacing a group's real arrays. Now refused with a message pointing at `--pack`.
  * `<dir> --pack` / `--loose` with no `<png-dir>` argument used to fall through the same way
    (Mountains too). Now refused instead of regenerating 15 placeholders over real ones.
  * The mode flag (`--arrays-only`/`--material`/`--pack`/`--loose`) must be `argv[2]`; given later
    (e.g. after `--group`) it used to be silently read as part of `--group`'s value and fall
    through to the placeholder path. Now refused with a position error for all four flags.
* Refactor only, no behaviour change: `packReal`'s per-layer file search (`findLayer` lambda) and
  per-layer map building (`buildSet` lambda) are pulled out as free functions `findLayerFiles` /
  `buildLayerMaps`, reused by the new `packGroupReal`. `writeGroupArrays`'s per-array build+save+
  readback is pulled out as `saveGroupArray`, reused by `packGroupReal` too.

## Verified

* **Build**: `landscape_tex_gen` and `he_tests` built clean (Release, `C:\hb\l8`, short path —
  the worktree's own path hits Windows' 260-char limit in `_deps`, same trap as earlier steps).
* **A/B against the pre-change binary** (`C:\hb\l7`'s `landscape_tex_gen.exe`, built at `4832517d`):
  Mountains placeholders, `--variants 3`, `--pack` (synthetic PNGs), `--material`, and Forest's
  placeholder arrays + `--material --group Forest` are all **byte-identical** (MD5) between the
  old and new binary — the refactor changed nothing for any existing path.
* **Functional, synthetic PNGs** (solid, distinct colours per role, `C:\tmp\{forest,grassland,
  desert}_pngs`): `--pack --group Forest` reads all 4 real role files, `--pack --group Grassland/
  Desert` with only `*_Albedo.png` present correctly falls back to placeholder Normal/Roughness/AO/
  Height per role (and now prints the group's own role name in the "missing" lines, not Mountains'
  `Grass`/`Dirt`/`Rock`/`Snow` — a bug caught and fixed during this run). Read the written
  `T_Landscape_Forest_Albedo_Array.hasset`'s `PIXL` chunk back by hand (slice-major, 6 baked mips at
  size 32): slice 0..3 are exactly the 4 roles' colours, slice 4 is the WetGround placeholder tone —
  no slice/role swap.
* **Guard rails**: `--arrays-only --group Forest`, `--pack --group Forest` with no dir, and
  Mountains' bare `--pack` with no dir all refuse (exit 2) instead of silently regenerating
  placeholders. Unknown `--group` name refuses too (pre-existing, unchanged).
* **Tests**: `tests/test_landscape_groups.cpp` (unchanged except one new line pinning the
  `kLooseGroupIndexBase` block `0x4A8..0x4CB` into the id-uniqueness check) passes, 4/4 cases,
  1040/1040 assertions. Full `ctest --test-dir C:/hb/l8 -j4 --timeout 180`: **348/348 (100%)**, run
  with `HE_NET_LOOPBACK_ONLY=1 HE_HIDDEN_WINDOW=1` and a scratch `APPDATA`/`LOCALAPPDATA`.

## Not verified

* The actual human textures, because none exist on this device yet (see above). Everything above
  used synthetic solid-colour PNGs as a stand-in for the real file names/aliases.
* EngineContent server upload (`HE_ContentSync`) — nothing to upload yet for the same reason.
* D3D11/D3D12/Vulkan/Metal runtime rendering of a `--pack`ed group array — this step only touched
  the generator tool and its tests, not the renderer or material-graph code (unchanged since the
  Windows acceptance in step 5), so no new backend risk; not re-run here.
* `--variants N` for a group's `--pack` (real texture variants per role) — not implemented; groups
  only get a single variant from `--pack`, same as `--loose` already was for Mountains. If a group
  ever needs photographic variants, follow Mountains' `_v2`/`_v3` suffix pattern in `packGroupReal`.

## Traps for next time

* **A full `he_tests.exe` run (not `ctest`) hung** at `test_engine_api_direct.cpp`'s lockstep test
  with near-zero CPU growth for 20+ minutes — but the *same* test passed in 0.09s when ctest ran it
  in isolation (`he_tests --source-file=*test_engine_api_direct.cpp`). Looks like state bleed from
  an earlier test in the same process when everything runs in one binary, not a real bug in this
  test. Logged as a Hive lesson. Prefer `ctest -j4 --timeout N` over the raw binary for a full run
  on this branch: a stuck file can't block the others, and it dodges whatever this is.
* The worktree's own path is long enough that a fresh CMake build tree under it hits the 260-char
  `_deps` path limit (`nlohmann_json-populate-stamp/...`); configure into a short path
  (`C:/hb/l8` here) instead, per the existing `-DDEPLOY_DIR` lesson for worktree builds.

## Handoff

* **Branch / commits**: `claude/landscape-3-0-textur-layer-gruppen-forest-grassland-mountain`,
  `src/HE_Tools/src/LandscapeTexGen/main.cpp` (+338/-122) and `tests/test_landscape_groups.cpp`
  (+4) plus this doc.
* **Decisions taken**: in-memory packing for groups (no new per-role `.hasset` files — the id
  block genuinely has no room, and the code already documented "a group has no per-layer files"
  as the design); new id block `0x4A8..0x4CB` for group loose maps, leaving `0x4A4..0x4A7` free for
  a next group; role-label-with-space-removed as the `<RoleLabel>` file-name convention (matches
  what the assignment itself suggested, e.g. `ForestFloor_Albedo.png`).
* **Pending (human, unanswered, carried over from steps 2/6)**: real Forest/Grassland/Desert source
  textures do not exist anywhere on this device. Until they arrive, running `--pack --group <G>`
  for real is not possible, and neither is the EngineContent upload this step was also asked to do.
* **Next steps**: once the human delivers PNGs (one flat folder per group, `<RoleLabel>_<Map>.png`),
  run `landscape_tex_gen EditorDeps/EngineContent/Textures/Landscape --pack <dir> --group <G>
  --size 2048` for each of Forest/Grassland/Desert, then `--material --group <G>` if the graph
  needs regenerating (it doesn't depend on array contents, only paths — unlikely to need a rerun),
  then publish via the Editor's "Publish Engine Content to Server" (`HE_ENGINE_CONTENT_EDITABLE=1`)
  and verify with the manifest-refresh log line + a headless `HE_DUMP_AUTOLANDGROUP=<G>` witness.
