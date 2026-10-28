# Landscape regression: which material asset costs what (Thema 237, Schritt 2)

22.10.2026, same MacBook Air M5 and scene as `landscape-performance-regression-investigation-2026-10-21.md` (Catania, Metal, 5.2 MP editor loop, GI on,
`--detailed`, 200 warm-up + 600 frames). **Investigation only, no source change, no build.** Raw data: `perf-audit/raw-landscape-regression/d22*`.

## 0. Result

1. **The +1.5 ms of Thema 180 (SSAO pass) is mostly `8f3a6e47`'s asset (+1.3 ms), `0d7cdf09`'s asset adds ~+0.35 ms.** Same engine (current `12be6adc`), only `M_AutoLandscape.hasset` swapped, 3 runs each, runs interleaved:

| Asset on the current engine | commit it is the version of | Σ passes mean (runs) | SSAO mean | Scene mean |
|---|---|---|---|---|
| `835331d7` (doc: `ac7f`) | `ca1f24c0`, Oct 9, before 180 | **23.79** (23.68 / 23.72 / 23.97) | 7.09 (7.05..7.15) | 13.73 |
| `bb0e9e31` (never deployed before) | `0d7cdf09`, Oct 10, puddles | **24.15** (24.57 / 23.72 / 24.15) | 7.32 (7.24..7.38) | 13.83 |
| `5439dc94` (doc: `dd4d`) | `8f3a6e47`, Oct 21, texture variants | **25.41** (25.23 / 25.56 / 25.45) | 8.60 (8.54..8.65) | 13.81 |

   Steps: ca1f24c0 -> 0d7cdf09 **+0.36 ms** (SSAO +0.23; Σ ranges overlap, SSAO ranges do not); 0d7cdf09 -> 8f3a6e47 **+1.26 ms** (SSAO +1.28; no overlap). Scene pass is flat in all three.
   The hashes are `sha256[:8]` (convention of `binaries.txt`) of the files; `sha1` of the git blobs is what `git log` shows.
2. **Oct 8 -> Oct 10 (+3.1 ms): half asset, half engine.** On the Oct 10 engine (`b1010a`, `f6e7452f`) the asset decides the SSAO pass, the engine decides the Scene pass:

| Cell | Σ mean (runs) | Scene | SSAO |
|---|---|---|---|
| `b1008b` (Oct 8 engine, `b218` asset = `e6aaee2d`) | 20.85 (21.18 / 21.07 / 20.29) | 12.11 | 5.88 |
| `b1010a` + `b218` asset (swapped) | 22.66 (22.87 / 22.92 / 22.19) | 13.94 | 5.85 |
| `b1010a` as built (`ac7f` asset = `ca1f24c0`) | 23.91 (24.20 / 24.17 / 23.36) | 13.96 | 7.08 |

   Asset `b218` -> `ac7f` on the same engine: **+1.25 ms, all SSAO** (Scene +0.02). Engine Oct 8 -> Oct 10 with the same asset: **+1.8 ms, all Scene** (SSAO -0.03). Not attributed further (see 3).
3. **What is not separable with these data:** (a) the Scene +1.8 ms of the Oct 8 -> Oct 10 *engine* (`b1008b` is another branch than `b1010a`; no clean commit series, no build made).
   (b) The mirror cell (Oct 8 engine with the `ac7f` asset) is **invalid**: that engine has no `MF_WeatherSnow/MF_WeatherPuddles` function assets, the log shows
   `Cannot load asset ... MF_Weather...` and `Pinned-MSL shader compile failed`, the cell reads 18.1..18.6 ms because the landscape did not render. Kept in `invalid-asset-not-loadable/`, not in any table.
   (c) Engine side of the Thema 180 +1.5: step 1 showed the new asset on the Oct 19 engine gives 7.25 (not 8.6), so the cost needs the 8f3a6e47 *engine* code too. `0d7cdf09` is **not** in the Oct 18/19
   engines (`git merge-base --is-ancestor 0d7cdf09 40c68ec7` = no), so that row does not tell which of the two commits' engine code is needed; only a build between them would.

## 1. Why the asset moves the SSAO column (inferred, not measured)

The pass booked as "SSAO" depends on the terrain material (step 1: 8.6 -> 2.1 ms with the built-in material), so it is where the landscape material's fragment cost lands. `8f3a6e47` adds the `Groups` pin and `heBombGridV`
(per bombed read: an extra `uvec3` per grid, integer variant pick `((h.x & 255u) * V) >> 8u`, slice = group * V + variant). The commit says no texture read is added, and the cache arrays have V = 1, so the 1.3 ms is
ALU/register cost of a variant selection that selects nothing today. That would make it paid for a feature (variants) the player's arrays do not use. Not verified by a shader dump.

## 2. How it was measured / what to trust

* No `matgen` build was needed: `M_AutoLandscape.hasset` is committed (`EditorDeps/EngineContent/Materials/`), `git show <commit>:<path>` gives each version; copies in `/tmp/he237/{cur_d906,b1010a_b218,b1008b_ac7f}`, hash-checked.
* Power: **AC adapter** for all d22 runs (step 1: battery); absolute values are not comparable with step 1's table (cur 25.41 vs 25.24 there), only inside this series. Load averages 2 to 6 (other bees), `foreign: none` in all kept runs
  but `d22d906-r2` (cmake 13.9 % at the end). Runs that overlapped a compiler were moved by the runner to `discarded-under-load/` and repeated.
* Interleaved order for A (cur, d906, oldmat per round); B was run cell by cell in rounds of four. Σ ranges inside a cell are <= 1.0 ms, the differences quoted are 0.36 to 1.3 ms: the 0.36 is near the noise, the 1.3 is not.

## Handoff

**Done (verified by runs):** attribution of the Thema 180 +1.5 ms to the two asset versions (section 0, n = 3 each, interleaved); Oct 8 -> Oct 10 split into asset (+1.25, SSAO) and engine (+1.8, Scene);
`landscape_regression_table.py` reproduces every step-1 row unchanged (diff of old and new table without `d22` rows is empty).
**Not verified:** why the variant code costs 1.3 ms (no shader dump, inferred); which Oct 8 -> 10 engine commit brings the Scene +1.8 ms; the engine-side split between `0d7cdf09` and `8f3a6e47` code; battery power; the
player's real backend (request 146, still the queen's); D3D/Vulkan/GL. No source file changed, so no `he_tests` build was made (the check does not apply).
**Decisions taken:** asset swaps instead of `matgen` builds (the asset is in git); labels `d22*` so the step-1 rows stay as they were; the invalid Oct 8 + `ac7f` cell kept apart in `invalid-asset-not-loadable/`.
**Only suggested:** an 8f3a6e47 follow-up that makes the variant path free when V = 1 (e.g. a static switch / separate compile path so an array with depth 5 compiles `heBombGrid`, not `heBombGridV`); needs the owner of Thema 180.

**Next steps, in order:**
1. Thema 180 owner: check whether `heBombGridV` with V = 1 can be compiled out (the shader knows only the texture depth at run time; the cache arrays are 5 deep). Measure with `cur_d906` vs `cur` as before (target: SSAO 7.3 ms again).
2. Oct 8 -> Oct 10 engine Scene +1.8 ms: `git log` between `0af31ea0` and `f6e7452f` for the Scene pass (clouds/sky/GI at Oct 9-10); build only if the log is not enough; the Oct 9 transient (`b1009`, 38 to 52 ms) shows the Scene cost can swing.
3. Still open from step 1: the backend question (request 146), the SSAO column ignoring `SSAOEnabled=false`, the clouds (12.7 ms), a GL series with >= 5 runs per build.

**Pitfalls (new):** `caffeinate -d -u -t N` did **not** keep the display on once the screen was locked and idle: the display went to sleep again 20 s after waking it, the editor then ran 600 frames in 8 s, never exited, and the
runner reported `TIMEOUT after 240 s` three times (about 40 minutes lost). `/tmp/he237/tools/keepawake.sh` (`caffeinate -u -t 6` every 4 s in a loop) held it; check with `CGDisplayIsAsleep` (python ctypes, CoreGraphics) before a batch.
A human's CLion build (`ninja -j 8`, 1 h) and other bees' editors blocked the runner's "quiet machine" wait for ~1 h; after 1800 s the runner starts anyway and the result is discarded and repeated: start batches when no ninja runs.
Old engines cannot load newer material assets (missing `MF_Weather*` function assets): a swap into an old build is only valid downwards (old asset into new engine, or an asset the old engine can load); check the log for `Cannot load asset` / `Pinned-MSL shader compile failed`.
Orphaned `HorizonEditor` processes after a TIMEOUT: `pgrep -fl "he237.*HorizonEditor"` and kill them.
