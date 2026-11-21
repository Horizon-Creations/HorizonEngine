# Landscape: three variants per ground/slope role in every texture group (2026-11-03)

Every texture group (Mountains, Forest, Grassland, Desert) had ONE texture per painted role (the arrays were
5 slices deep, `V = 1`). The shader already draws one of `V` variants per hex of the texture bombing
(`docs/auto-landscape-material-2-variants-2026-10-21.md`), `landscape_tex_gen` just could not pack variants for a
group. Now every role that is read from the arrays has **3 variants** (13-slice arrays, 2048², 12 mips), with
Poly Haven CC0 textures, and the Desert got better (and matching) rock/hardpan sets.

## What changed

* `landscape_tex_gen <out> --pack-group <png-dir> --group G [--size N] [--compress bc7|astc]`: the group-array form of
  `--pack` for EVERY group, Mountains included (`packGroupReal`, `src/HE_Tools/src/LandscapeTexGen/main.cpp`).
  `<RoleLabel>_<Map>.png` is variant 1, `_v2`, `_v3` ... the others (a role with no such file repeats variant 1, which
  is how the procedural roles — Snow everywhere, Desert's Sand — keep their placeholder). Writes no per-layer loose
  files. Array ids: `groupArrayId` (Mountains' arrays are `0x40F..`, not behind its material id).
* `--compress bc7|astc`: the array leaves the tool already block-compressed, via the new `Hpak::cookTextureBlob`
  (`HpakWriter.h/.cpp`): the export's own array cook (every slice/level as it stands, slice means in `CHUNK_TXSM`).
  The slice encoding now runs on all cores (`encodeChains`), a 13-slice BC7 array takes seconds instead of minutes.
  `--mix` copies compressed slices and their means too. **Platform-specific, not for shared EngineContent** (below).
* `src/HE_ContentSync`: `publishEngineContentFilesBlocking` / `verifyEngineContentFilesBlocking` and the tool
  `content_sync_put put|verify <root> <rel>...`. The editor's full publish uploads the manifest of the LOCAL scan, so
  from a root that holds only part of the library it would **delist everything else** (the server manifest holds 99
  entries; `Textures/` is not in the repo any more). `put` uploads exactly the named files and MERGES them into the
  remote `manifest.json` (and refuses if that cannot be read). The credentials stay compiled into the DLL, nothing is
  printed.
* `scripts/landscape-textures/variant_sets_download.py` (plan + download, 2k, one folder per set) and
  `variant_sets_stage.py` (names the files for `--pack-group`; cuts Mountains' variant 1 out of the CURRENT server
  arrays so repacking keeps what was there).
* `tests/test_texture_bcn.cpp`: `cookTextureBlob` (array becomes BC3 in place, slices stay in order over 8 slices on the
  worker threads, means baked, unknown format / already compressed returned untouched).

## The sets (all CC0, https://polyhaven.com/a/<id>, 2k: Diffuse jpg, Normal GL png, Rough/AO/Displacement jpg)

| Group | Role | v1 | v2 | v3 |
|---|---|---|---|---|
| Mountains | Grass | existing (server) | grass_path_3 | grass_ground |
| | Dirt | existing | brown_mud_dry | park_dirt |
| | Rock | existing | rock_face_03 | rock_05 |
| Forest | Forest Floor | forest_ground_04 | brown_mud_leaves_01 | brown_mud_rocks_01 |
| | Roots | forrest_ground_01 | forrest_ground_03 | forest_leaves_02 |
| | Mossy Rock | mossy_rock | rock_3 | rock_face_03 |
| Grassland | Meadow | grass_path_2 | grass_ground | grass_path_3 |
| | Bare Soil | brown_mud_03 | brown_mud_02 | brown_mud_dry |
| | Stones | rocks_ground_03 | pebble_ground_01 | rock_ground_02 |
| Desert | Hardpan | sandy_gravel_02 | dry_ground_01 | dirt_floor |
| | Sandstone | cliff_side | rock_06 | dry_riverbed_rock |

Variants were picked for **similar mean luminance** within a role (measured on the diffuse): the first pick
(mud_cracked_dry_03, rock_boulder_cracked/_dry, grass_medium_01 — that one is a foliage card, luma 0.013) gave a harsh
camouflage mosaic because the shader switches variant per hex. Desert's v1 replaces the old Hardpan/Sandstone
(the sandstone was grey chunks); `docs/img-landscape-slope-variants/groups-d3d11-bc7-v3.jpg` shows the four groups.

## Server format: RGBA8 masters, compression is per platform

`.hasset` content on the server is shared by every editor, so it is the neutral RGBA8 form (`Assets.h`: "RGBA8 is the
raw/editor form, the others are pack-time cook targets"); BC7 is no Apple format (Metal takes ASTC), so BC7 files on
the server would show flat landscapes on a Mac. Exports already cook arrays per target (`cookTextureArrays` is on in
`ProjectExporter`, BC7 desktop / ASTC Apple, slice means baked). Cost of that: 277 MB per group array (V = 3, three
arrays per group), 341 MB per Mix array, ~4.3 GB for the 15 files, and the same in the editor's VRAM — see Next steps.
BC7 files were produced and tested too (69 MB per array), ASTC for the Desert (the encoder works, 46 s for 3 arrays).

## Upload (2026-11-04, `content_sync_put put`)

15 files replaced under `Textures/Landscape/` on the EngineContent server: `T_Landscape_{Albedo,Normal,Mask}_Array`
(Mountains), `T_Landscape_{Forest,Grassland,Desert}_{Albedo,Normal,Mask}_Array` (290.8 MB each, 13 slices) and
`T_Landscape_Mix_{Albedo,Normal,Mask}_Array` (357.9 MB, 16 slices; variant 0 of each group role, rebuilt from the new
group arrays so the mix does not show the old textures). About 4.3 GB in 33 minutes (~2 MB/s). The remote manifest
was merged (99 entries before and after); `content_sync_put verify` afterwards: remote size == local size and the
manifest hash matches for all 15. A copy of the manifest as the editor cached it before the upload is not in the repo.
Editors pick the new files up through the normal manifest sync (hash changed, re-download).

## Handoff

* **Topic / step:** landscape slope/ground variants, one step. Branch `release/0.8.0`, commit below (not pushed).
* **Done:** all four groups have 3 variants per read role (Poly Haven, table above); the 15 RGBA8 arrays (12 group +
  3 Mix) built with `--pack-group` / `--mix` and uploaded + verified on the server (see "Upload").
* **Files:** `src/HE_Tools/src/LandscapeTexGen/main.cpp` (`packGroupReal`, `groupArrayId`, `saveGroupArray`
  compress, `writeMixArrays` means), `src/HE_Core/src/Hpak/HpakWriter.cpp` (`encodeChains`, `cookTextureBlob`),
  `src/HE_ContentSync/{src/EngineContentPublish.cpp,tools/content_sync_put.cpp,CMakeLists.txt}`, `tests/test_texture_bcn.cpp`.
* **Verified:** Release build in `C:/hb/tex` (`-DDEPLOY_DIR` away from the user's deploy); `he_tests`
  `test_texture_bcn` 16/16, `test_landscape_groups`, `test_hpak`, `test_contentmanager` green. Witness
  (`scripts/landscape-3-0-accept/cap233_win.ps1`, copy of the deploy, hidden window, camera `CAMX=-62 CAMY=326
  PITCH=-24 YAW=90`, ramp 30/56): all four groups render with no missing-asset log on D3D11; the Desert's BC7 arrays
  on D3D11/D3D12/Vulkan/OpenGL agree to mean |d| 0.00-0.01; BC7 vs RGBA8 arrays: Desert 0.07, Forest 1.3 (max 41).
  `content_sync_put verify` against the cache: server arrays == cache bytes before the upload (so variant 1 of
  Mountains is the shipped one).
* **Not verified:** Metal/ASTC rendering (no Mac), the Mix material with the new arrays (the arrays load, the mix
  witness `HE_DUMP_AUTOLANDMIX` was not run), perf (V changes slices, not taps), Forest/Grassland v1 were placeholders
  or unknown before (the cache never held them), not looked at.
* **Decisions taken:** V = 3 for all groups (user); Mountains Rock variants are plain random variants (the
  `claude/landscape-3-0-folge-...` branch, not in release/0.8.0, uses them as large/medium/small by slope: reorder
  when that merges); RGBA8 on the server (user, after the Apple point).
* **Decisions suggested, not taken:** the editor should upload block-compressed arrays itself (user's wish) — nothing
  does that today; Snow/Sand slices are wasted placeholders (`4V+1` layout), a layout without them would save 3-6 of 13.
* **Traps:** `landscape_tex_gen` mode flag first (`--pack-group` is one); Windows MAX_PATH breaks
  `cap233_win.ps1` from the scratchpad (use `C:/hb`); never publish a partial root with the editor's full publish
  (manifest replaced); `python -` without stdin hangs the shell tool.
* **Next steps:** 1. editor-side compression at GPU upload (BC7 desktop / ASTC Apple, cached on disk: `cookTextureBlob`
  is the building block, needs a format choice per device in `ContentManager`/renderer and a cache next to
  `EngineContentCache`); 2. Metal/ASTC witness on a Mac; 3. tune Rock Slope etc. against the new textures.
