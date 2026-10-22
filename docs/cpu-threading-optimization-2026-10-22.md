# CPU threading: foliage extract and scatter on the pool (Thema 238, Schritt 2)

22.10.2026, MacBook Air M5 (10 cores), **Metal measured, OpenGL picture-checked, D3D11/D3D12/Vulkan not touched or run** (the changed files are
shared code, see section 5). Follows `cpu-threading-audit-2026-10-22.md` (Schritt 1). Raw reports: `perf-audit/raw-cpu-threading/step2/`
(`summary.txt` = every run, `*.report.txt` = `thread_timeline_report.py` output).

## 1. What changed (all default-on, each with an off switch)

| Item | Where | Switch |
|---|---|---|
| Straddler buckets (the ones the draw distance / a level band cuts through, tested plant by plant) run as pool jobs into their own buffers; the main thread appends and emits them in layer, straddler, level order | `FoliageExtract.cpp` `planBuckets` :457, `runStraddler` :488, `emitPlans` :561 | `HE_FOLIAGE_PARALLEL=0` |
| A standing camera replays the previous extract (key = camera, per layer entity/flags/distances/terrain pose/levels incl. mesh reach, plus store identity; recorded from the 2nd identical extract so the hysteresis memory has settled) | `FoliageExtract.cpp` `addToKey` :681, `extractFoliage` :786, `FoliageLodMemory::Replay` (`FoliageExtract.h:71`) | `HE_FOLIAGE_MEMO=0` |
| Dirty foliage layers are scattered as pool jobs (same per-layer RNG stream, same bytes) | `FoliageSystem.cpp:311` `update`, `parallel_for` :356 | none (1 layer runs inline) |
| Profiler scopes only: `ParseAndRegisterAsset`, `WarmupWorldMaterials`, `Metal::SkyEnvRebake`, `FoliagePlanBuckets`/`FoliageStraddlers`/`FoliageEmitClusters`/`FoliageScatterLayer` | `ContentManager.cpp:175`, `EditorApplication.cpp:14428`, `MetalRenderer.mm:13893` | |

Output is identical, not "close": `out.objects`, `out.instanceBlocks`, the scratch vector and the level memory come out as the one-thread walk gave them
(`tests/test_foliage_parallel.cpp` compares the pool walk with the one-thread walk bit for bit over a camera path that crosses level boundaries, on a
rotated terrain too, and the replay against a walk over stops with every kind of change in between).

## 2. Measurements (Metal, game dir without pak, static camera, medians of 3 interleaved runs, 250 frames after 120 warm-up)

`ExtractFoliage` p50 / main-thread WORK p50 (per frame, `NextDrawable` subtracted by the report script), ms:

| Scene | before | new (default) | memo off, pool | memo off, one thread |
|---|---|---|---|---|
| `s512_x4` (425k stored, 9k drawn) | 0.274 / 1.07 | **0.014 / 0.82** | 0.173 / 1.00 | 0.343 / 1.15 |
| `s128_x16` (102k x16) | 0.798 / 2.15 | **0.011 / 1.34** | 0.406 / 1.76 | 0.915 / 2.25 |
| `ref10k` (10k meshes, no foliage) | `ExtractFrame` 1.415 / 4.30 | 1.413 / 4.32 | | |

* "memo off" is what every frame of a MOVING camera costs (the walk does not depend on the movement): **-37 % (s512_x4), -49 % (s128_x16)** against
  the old serial walk. The new serial path (switch off) is 14 to 25 % slower than the old code (per-straddler buffers, merge copy): it is only the
  fallback for `HE_FOLIAGE_PARALLEL=0` and a pool that is busy.
* One-time scatter (frame 0, `Foliage` scope, `s512_x4`, 524k instances): **55.8 / 56.3 / 50.2 ms before, 31.9 / 31.4 / 30.7 ms after** (the biggest
  layer is the floor, the rest runs beside it).
* `ref10k` has no foliage: it is the regression guard and did not move.
* **The audit's baselines were too high.** Schritt 1 reported 0.77 ms `ExtractFoliage`, 124 to 142 ms scatter and a 2.59 ms game WORK. The first run
  of this session (a build of this tree next to the other bee's editor) gave 0.772 ms again; the same binary on a quiet machine gives 0.27 ms, 50 to
  56 ms and 1.07 ms. Shares quoted in the audit overstate the foliage cost by about 2.8x; the ranking of the items is unchanged but their absolute gain
  is smaller than the audit's table says.
* A moving-camera bench exists (`he_tests --no-skip -tc="*Foliage extract bench*"`, 524k instances, one thread vs pool, alternating per step): 0.73 vs
  0.43 ms whole extract on a quiet machine; it was inconclusive while the machine ran at load 11 to 14 (other bees building), so it is not quoted above.

## 3. Verification

* **Metal picture, default mode: bit-identical.** `HorizonGame` screenshot (frame 200, `HE_SKY_TIME=30`) of `s512_x4`, old vs new binary, md5 equal for the
  default, `HE_FOLIAGE_MEMO=0`, `HE_FOLIAGE_PARALLEL=0` and both off; also `s128_x16` default, `HE_FOLIAGE_CLUSTERS=ordered` and `=0` (control: the old
  binary twice gives the same md5).
* **`foliage_pixel_ab.py` (editor, 6 cases incl. 500k, Metal and OpenGL): the `default clusters` column is `identical` in all 12 runs.** The script's exit
  status is 1 and says FAILED: that is the script's old premise (BEFORE built before the cluster path), here BEFORE already has clusters, so `=0` and
  `ordered` of the new editor differ from the BEFORE *default* picture by the known depth-tie pixels (751 to 3730 on Metal). Those two modes were not
  touched; their equality with the old binary is shown by the game md5s above. Do not read the exit status as a regression.
* `he_tests`: `test_foliage_parallel` (4 cases, 168k assertions), `test_foliage_cluster`, `_layers`, `_lod`, `test_layer_mesh_instancing`, `test_culling`
  (sky noise digests), `test_async_streaming` (13/13) and `test_hpak` (59/59 with `HOME=<empty dir>`) pass. Full `he_tests` run: 5686 of 5688 pass; the two
  failures are `test_profiler.cpp:107` and `:215` (order-dependent global profiler state, pass alone, 18/18), and **the pre-change binary of the neighbour
  worktree (6479c3d1) fails the same two** in the same full run.
* Not exercised: a pak with real engine content (item 7), the editor's cold start (item 6), moving camera in the game (harness is static).

## 4. Not done, with the reason

* **SkyNoise3D cache: dropped.** Re-measured: `BuildSkyNoise3D(256)` is 200 ms wall / 845 CPU-ms on a quiet machine (8 threads: the caller plus 7 `std::thread`,
  confirmed by the header and the code; the audit's 1.56 CPU-s was taken under load). A cheaper Worley (distance bound that skips cells, same arithmetic per
  evaluated cell, digests in `test_culling` still pass) was **slower, 1070 vs 845 CPU-ms**, and was reverted. What is left is a disk cache: 64 MiB per user
  and launch of every game, a version contract with the generator. That is a product decision (where may a player's machine keep 64 MiB, or ship it in
  the pak), not a tuning step; nothing was written.
* Pre-resolved bounds table for the extract gather (audit item 2), pak/async asset path (item 7), per-index counter RNG (changes every scene's layout),
  sky rebake async (item 5, only the scope was added): not started, as scoped.
* The editor's cold 6.7 s (item 6) still has no cause; `WarmupWorldMaterials` and `ParseAndRegisterAsset` scopes now exist so the next editor capture names it.

## 5. Backends

`FoliageExtract.cpp`, `FoliageSystem.cpp` and `ContentManager.cpp` are shared code, compiled once into `libHorizonEngine`; there is no per-backend
variant. **Metal** (measured and picture-checked), **OpenGL** (picture-checked with `foliage_pixel_ab.py --rhi OpenGL`, default column identical).
**D3D11, D3D12, Vulkan: not built, not run** (this Mac cannot; they read the same `RenderWorld`, and the output of the extract is bit-identical, so
nothing they consume changed). The only backend file touched is `MetalRenderer.mm` (one profiler scope).

## Handoff

- **Topic / step:** Thema 238, Schritt 2. Branch `claude/cpu-threading-audit-jobsystem-auslastung-daten-vorbereitung-`, code in `d97b5bfc`, doc + raw reports in the commit after it.
- **Verified (how):** section 3. Screen was **locked** the whole time (`CGSSessionScreenIsLocked=Yes`, cannot be changed from here): `NextDrawable` is subtracted by the report script, WORK is CPU time. One run stalled for ~25 min mid-session (probably system sleep; `caffeinate` afterwards); runs overlapping other bees' editors/builds were repeated or excluded (first scat/`b_s512_x4` runs, a load-11 bench).
- **Not verified:** D3D11/D3D12/Vulkan; moving camera in the real game; any pak/engine-content run; editor cold start; `HorizonEditor` was built (rc 0) but not driven by hand.
- **Decisions taken:** replay keyed on the values the walk reads rather than on `FoliageComponent::revision` (revision does not cover the meshes' bounds becoming known, nor the transform); recorded from the second identical extract; straddler buffers kept per view (`FoliageLodMemory::buffers`) to avoid per-frame allocation (not measured separately, bench too noisy); SkyNoise cache dropped (section 4).
- **Decisions suggested, not taken:** if the 64 MiB sky volume matters, ship it as an engine asset or cache it per user; make the serial fallback cheaper (merge copy) if pool-less configurations matter; pre-resolved bounds table (audit item 2) is the next real main-thread win (`ExtractFrame` 1.4 ms at 10k meshes).
- **Traps:** run `thread_timeline_report.py` only on a quiet machine (`pgrep HorizonEditor`; this session's other bee spawned editors every ~2 min); `foliage_pixel_ab.py` exit status 1 is expected when BEFORE already has clusters; a replayed extract is only recorded on the 2nd identical call, so tests need 3 extracts per stop; a new input of the foliage walk must go into `addToKey`.
- **Next steps:** (1) CI matrix run for Windows/Linux (D3D/Vulkan only compile there); (2) decide the sky-noise cache question; (3) re-measure with the screen unlocked and a moving camera script; (4) audit item 2.
