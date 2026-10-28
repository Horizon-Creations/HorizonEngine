# CPU threading: per-step algorithmic optimization (Thema 238, Schritt 5)

27.10.2026, NN-WS03 (Windows, AMD Ryzen 9 9950X, 16C/32T), **D3D12** (also D3D11 build available;
**no Metal on this machine** — a deviation from the brief's "single Metal backend", posted to the
topic as a warning before this doc was written). **No engine code changed.** New: `he_tests`
Release build in `C:/hb/cta5` (short-path override of `out/build/x64-release`, MAX_PATH), a Release
`HorizonGame` D3D12 build, two he_tests benches and three real `HE_PROFILE_CAPTURE` dumps, raw logs
in `docs/perf-audit/raw-cpu-threading/win-*-2026-10-27.{csv,stdout.txt}`, this doc.

Builds on `cpu-threading-audit-2026-10-22.md` (Schritt 1, 8 hot spots on Metal/M5) and
`cpu-threading-dependencies-2026-10-24.md` (Schritt 4, task graph). This step looks at each of
those 8 items **again, one at a time, for a LOCAL (non-parallel) shortening**: algorithm,
data structure, unneeded work, early exit, caching/precompute — not "can it run on the pool"
(that is Schritt 2/6).

## 1. What changed under this audit since Schritt 1/2 (read before trusting any old number)

The branch's `HEAD` (`d09e65ca`) is release/0.7.0 plus this topic's own commits, and release/0.7.0
has absorbed a **separate** topic's work (`mac-frame-levers`, PRs #186/#194/#198/#199) that touched
the same files audit item 2 is about: `RenderExtractor.cpp` (`c38943ad` "extractMeshes runs wide",
`498de938` "mesh memo, wide RefineBounds, counting-sort sortByMesh", `56b4fbec` "Wide transform
scan, material scalars and shadow scene box"). Read at `RenderExtractor.cpp:349-390`: 3 wide
phases over 2048-entity blocks (filter, build in place into `out.objects`' prefix-summed slots,
multi-section fix-up serial only where a load may happen) — **audit item 2 is already done.**
Confirmed with a fresh capture, not just a read: see §2, item 2.

`TransformHierarchy::propagateTransforms` (`TransformHierarchy.cpp:450-520`) — a candidate named in
the Schritt 4 doc ("dirty-only transform propagate") — **is already dirty/subtree adaptive**: full
walk only when the structure epoch moved or `forceFull`, otherwise a scan flags changed entities and
climbs only their subtrees, with hysteresis (`preferFull`) back to a full walk when most of the
world is flagged. Also confirmed with a fresh capture (§2, item 9): predates topic 238, nothing to do.

## 2. Table

Confidence: **M** = measured this run (Windows, D3D12 game capture or backend-independent he_tests
bench); **MS** = measured by an earlier step/platform (Mac/Metal) and not re-verified here — the
ranking is transferable, the absolute ms is not; **E** = estimated from a code read, no run.

`% of frame` uses `frame cpuMs p50` **minus** `WaitFrame p50` as the denominator — checked in code,
not assumed: `Render` (`Application.cpp:867`) wraps `m_renderer->Render()`, and `WaitFrame`
(`D3D12Renderer.cpp:13148`, "the CPU idles here when the GPU is the slower side") sits *inside* that
call, inside `DrawViewportFrame`. So `WaitFrame` is nested in `Render`, which is nested in the
frame's own `cpuMs` — it is **not** already excluded. WORK = cpuMs − WaitFrame: `s512_x4`
1.32 − 0.80 ≈ **0.52 ms**; `ref10k` 2.21 − 0.81 ≈ **1.41 ms**. (This mirrors Step 1's Metal
methodology — `NextDrawable` subtracted — once the subtraction is actually done.) Full numbers:
`win-d3d12-capture-2026-10-27.{csv,stdout.txt}`.

| # | Step | File:line | Measured | Local candidate | Speedup est. | Effort (1-5) | Risk/pitfall | Conf. |
|---|---|---|---|---|---|---|---|---|
| 1 | `ExtractFoliage` walk — bucket plan + straddler test (memo+pool already landed, Schritt 2) | `FoliageExtract.cpp:457` `planBuckets`, `:488` `runStraddler`, `:561` `emitPlans`, `:790` `extractFoliage` | **M (D3D12, `s512_x4`, 524k instances, static camera, steady state):** `ExtractFoliage`/`Extract::foliage` p50 **0.010 ms = 1.9% of WORK** (0.52 ms, cpuMs − WaitFrame) — matches Schritt 2's Mac "0.011-0.014 ms memoized (still camera)" closely, cross-platform. **M (he_tests, moving camera, same instance count):** whole extract one-thread **0.488 ms**, pool **0.244 ms** median of 600 — in the same range as Schritt 2's Mac "one thread 0.343-0.915 ms". `FoliageStraddlers`/`FoliageEmitClusters` did not appear in the static-camera dump (expected: nothing straddles once memoized) | `runStraddler` (`:488-557`) allocates a fresh `std::vector<uint8_t> levelOf(b.count, kUnknown)` (`:529`) **every call** — on a moving camera, every straddler bucket of every frame. `r.mats`/`r.clusters` are already reused via `StraddlerBuffers::results` (`:570-583`, cleared not freed) but `levelOf` is a plain local, so it heap-allocs+frees at the rate buckets are re-tested | small (allocator overhead only; not isolated in either run, since both were static-camera/memoized or the he_tests bench's bucket sizes made it noise-level) | 2 | `levelOf` is sized to `b.count`, which varies per bucket; reusing it means `assign`-to-size instead of a fresh vector — cheap, and must not leave stale bytes past the new, smaller `b.count` for the next, larger bucket | M (extract numbers) / E (the `levelOf` candidate itself) |
| 2 | `extractMeshes` gather (used to call `ensureResident` per entity) | `RenderExtractor.cpp:271-452` | **M (D3D12, `ref10k`, 10000 meshes, static camera, steady state):** `RenderExtractor::extract` p50 **0.254 ms = 18.0% of WORK**, `Extract::meshes` p50 **0.191 ms = 13.5% of WORK** (1.41 ms, cpuMs − WaitFrame). Schritt 1's 2.16 ms (Mac, pre-rework code) was itself ~2.8x inflated by a parallel bee (Schritt 2's own finding); its quieter-machine number for the same old code was `ExtractFrame` **1.413 ms**. Different CPU, different backend, so no precise multiplier — but a clear drop, not noise | **Already implemented** by the frame-levers topic (wide filter/build/fix-up, mesh memo, counting-sort `sortByMesh`); the fresh number confirms it, nothing left for Schritt 5 to propose here | done (lower on every available baseline, exact ratio not meaningful across machines) | n/a | none — re-baselining this (not re-optimizing it) was the risk, and it is now done | M |
| 3 | `FoliageSystem::scatterLayer` (per-instance RNG + placement rules + pose), one-time cost per dirty layer | `FoliageSystem.cpp:185`, hot loop `:249-299` | **M (D3D12, `s512_x4`, 524285 instances across 5 layers, one-time, `HE_PROFILE_WARMUP=1` run):** the `Foliage` wrapper scope (`SceneSystems.cpp:143`, waits for every layer's pool job) is **23.68 ms wall** — the number comparable to Schritt 2's. `dump_scope_p50.py` only reads the dump's main-thread `cpu` list, so the single `FoliageScatterLayer` call it shows (23.66 ms) is layer 0 (ran on Main per the log; layer 1 ran on `Worker-4` and is not in that list, not "the slowest of 5" by inspection, just the one the main thread happened to run). Schritt 2 (Mac): `Foliage`-scope-equivalent 55.8/56.3/50.2 ms before → 31.9/31.4/30.7 ms after (pool) — the fresh Windows number (23.68 ms) is faster than Mac's "after", consistent with a faster CPU, no regression | `glm::translate(mat4(1)) * glm::rotate(...) * glm::scale(...)` (`:291-293`) is **not** three naive 4x4 multiplies — glm's `translate`/`rotate`/`scale` (`matrix_transform.inl:10-86`) are already specialized for an affine chain (≈60 multiplies + 2 trig + 1 `normalize`, not ≈192). A hand-written direct TRS construction could still shave the redundant `normalize((0,1,0))` (the rotation axis is a compile-time-known unit vector here) and skip rebuilding the 3x3 `Rotate` matrix before combining it, but the ceiling is single digits of percent of this step's cost, not 10-20% | low single digits % of `scatterLayer`'s per-instance cost (corrected down from an earlier, wrong ≈192-multiply estimate — verified against the vendored glm source, see `_deps/glm-src/glm/ext/matrix_transform.inl`) | 2 | **Lower priority than first thought.** It can plausibly be made **bit-identical** (not just equivalent) if the replacement mirrors glm's exact per-term expressions, since axis=(0,1,0) makes most of `rotate`'s general formula collapse to exact 0/1/c/s terms the same way glm's own arithmetic does (`FoliageSystem.cpp:287-290`'s comment already reasons this way for the translate/scale order) — the bit-identical pixel-A/B and `test_foliage_parallel` tests are the check, not a blocker to route around | E |
| 4 | `SkyNoise3D::BuildSkyNoise3D` | `SkyNoise3D.cpp:10-83` | MS (Mac, Schritt 1/2): 845 CPU-ms / 200 ms wall at n=256, already thread-fanned (`:75-81`) | **Already tried and reverted** (Schritt 2): a cheaper Worley (cell-skip distance bound) was *slower* (1070 vs 845 CPU-ms). The 48³ jitter precompute (`:24-34`, comment cites "~453M at n=256") is itself a prior fix (perf audit B7, pre-238). Nothing further found; a disk cache remains a product decision, not a tuning step | n/a | n/a | do not re-propose the Worley rewrite, it was measured slower | E |
| 5 | `UpdateSkyEnvCube` rebake | `MetalRenderer.mm:13887` (D3D11 `:4712`, D3D12 `:9227`, Vulkan `:3055`, GL `:76`) | — | **Out of scope** (GPU time, excluded by this step's brief) | — | — | — | — |
| 6 | Editor `warmupWorldMaterials` / cold-start load | `EditorApplication.cpp:2288` | — | **Out of scope** (editor-only, excluded by this step's brief) | — | — | — | — |
| 7 | `ContentManager::parseAndRegisterAsset` over the pak/async path | `ContentManager.cpp:169` (scope at `:175`, added Schritt 2) | — | **Out of scope** (async/pak path, explicitly excluded — "out of scope per handoff run 1155") | — | — | — | — |
| 8 | `SceneJsonParse` fan-out / `CellStreamer` slice build | `SceneJsonParse.cpp:252`, `CellStreamer.cpp:551-634` | MS (Mac, Schritt 1): parse 102 ms parallel / build 75 ms serial at 10k entities; `CellStreaming` main-thread scope 120 ms over 500 frames (p50 0.015 ms). Not re-measured this run (no cells game dir built — see Handoff) | Read `CellStreamer.cpp:551-634` again for this step: the build is already nearest-first, slice-by-slice, time-budgeted (`overBudget()`, `:559-562`), a mid-build cell finishes rather than being cut — matches Schritt 1's own conclusion ("nothing to gain here"). The one thing not already tight: `bodiesFit` deferral does a `std::any_of` scan over all of `m_pending` (`:583-585`) per deferred cell check, O(pending) instead of O(1), but `m_pending` is bounded (tens, not thousands) | ~0 | n/a | none found worth the risk of touching a correctness-sensitive streaming budget | E |
| 9 (new) | `TransformHierarchy::propagateTransforms` — named as a candidate in Schritt 4, never measured | `TransformHierarchy.cpp:450-520` | **M (D3D12, `ref10k`, 10000 entities, static camera, steady state):** `Transforms::propagate` p50 **0.025 ms = 1.8% of WORK** (1.41 ms, cpuMs − WaitFrame); `Transforms::full` **did not appear** in the dump — the scan+subtree path handles every frame, the full walk never triggers once the scene has settled | Already dirty/subtree-adaptive (`:466-520`): a full walk only on structure change, otherwise a scan flags moved entities and climbs only their subtrees, with hysteresis back to full when most of the world moved. This predates topic 238 (`:434-448` `propagateTransformsFull` is the fallback, not the steady-state path) | 0 (already near-zero at 10k static entities) | n/a | none — this item is closed, not open | M |
| 10 (new) | `TerrainSystem::buildChunk` mesh generation — never measured in a scene that streams terrain in, only estimated in Schritt 1 | `TerrainSystem.cpp:318-394`, `TerrainMeshGenerator.cpp:378` `generateTerrainChunkMesh` | **M (D3D12, `s512_x4`, 16 chunks, one-time, `HE_PROFILE_WARMUP=1` run):** `Terrain` scope **5.19 ms** for "rebuilt 16 of 16 chunks" (log line, `TerrainSystem: rebuilt 16 of 16 chunks (all), res 257`) — in the same range as Schritt 1's Mac estimate (7.7-11 ms/16 chunks) | Each of `g.numLODs` levels (65, 33, 17, 9, … verts/side, `TerrainSystem.cpp:346`) independently resamples the **same** height field via `sampleField`/`sampleFieldNormal` (`TerrainMeshGenerator.cpp:384-388`) — but LOD(k+1)'s grid is an exact stride-2 subset of LOD k's (`verts = lod0Cells>>k + 1`). Downsampling LOD0's already-computed heights/normals into the coarser levels instead of resampling the field would remove most of the redundant finite-difference normal samples | ~20-30% of this 5.19 ms (not isolated per-LOD; the coarser levels are already cheap by construction, geometric falloff ≈1.35× a single LOD0 pass) | 3 | Touches shared mesh-gen code used by every terrain in every scene; the stride-2 grid-coincidence assumption holds for every current call site but would silently stop applying if a future caller passed an unrelated `vertsPerSide` — needs a guard that falls back to full resampling. This is a **streaming/load-time** cost (16 chunks once), not per-frame — lower priority than anything that runs every frame | M (the number) / E (the candidate) |
| 11 (new) | `SceneSystems::tickAnimation` / `AnimationSystem::update` — **the gap Schritt 1/4 explicitly flagged** ("no animated scene measured") | `SceneSystems.cpp:230`, `AnimationEval.cpp:27` `findBracket`, `:55` `applyChannel` | **M (he_tests, `HE_ANIM_BENCH=1`, backend-independent crowd bench):** linear in crowd size, **3.85-4.00 µs/character** (65 joints, 31-key full-body clip) at every size tried (1, 50, 200, 500, 1000) — `win-anim-bench-2026-10-27.csv`. 1000 characters = **3.85 ms** of serial main-thread work — as a share of a 60 fps (16.7 ms) frame budget, **23%**, with no other per-frame cost yet subtracted (this run has no renderer attached, so there is no real "frame" to divide by, only the budget a frame has to fit in) | `findBracket` already runs an **adaptive** linear-vs-binary-search keyframe lookup (`kBracketBinaryFrom = 64`, comment cites an earlier audit, `docs/audit-scene-systeme-2026-10-18.md`, that measured the crossover on the M5) — a pre-238 optimization, not something this step found newly broken. No further single-step shortening found: `sampleClip` already skips a channel whose `jointIndex` is out of range (`:134`) | 0 (nothing found to shorten) | n/a | the real lever here is parallelization (one thread per skeleton group, or per-character jobs) — Schritt 2/6 scope, not this step's. This result is the one new number this step was specifically asked to produce (perf-bookkeeping, Thema 238) | M |

## 3. Priority (effort vs. measured/estimated benefit, local-only)

1. **#2 — close it out.** Confirmed by a fresh D3D12 number (18.0%/13.5% of WORK vs. the old code's
   ~39% share), not just a code read. Nothing to do; stop quoting the old Mac number.
2. **#9 — close it out.** Same: a fresh number (1.8% of WORK, `Transforms::full` absent from the
   dump) confirms the dirty-tracking already works. Nothing to do.
3. **#11 Animation — no action, but now has a number.** Fills the exact gap Schritt 1/4 flagged.
   3.85 ms serial at 1000 characters is real budget if a scene has a crowd; the algorithm itself is
   already tight (adaptive keyframe search), the fix is parallelization (Schritt 2/6).
4. **#1 `levelOf` scratch allocation** — small, safe (same reuse pattern the rest of the straddler
   path already uses), only pays while the camera moves; the extract itself is already cheap
   (1.9% of WORK when memoized, confirmed on D3D12).
5. **#10 TerrainSystem LOD reuse** — real (5.19 ms measured for 16 chunks), modest ceiling, touches
   shared code, streaming-time only.
6. **#3 FoliageSystem TRS construction** — demoted from the original draft of this doc: the naive
   "three 4x4 multiplies" estimate was wrong (glm's `translate`/`rotate`/`scale` are already
   specialized, ≈60 multiplies not ≈192), so the ceiling here is low single digits of percent, not
   10-20%. Below #10 now.
7. **#4, #8** — nothing to do (already tried/rejected, or already tight). **#5/6/7** — out of scope.

## 4. Handoff

- **Topic/step:** Thema 238, Schritt 5 (global step id 1202). Branch
  `claude/cpu-threading-audit-jobsystem-auslastung-daten-vorbereitung-`, this doc + raw logs, no
  engine code.
- **Reproducibility:** `he_tests` + `HorizonGame` Release, Ninja/MSVC, `C:/hb/cta5` (short-path
  build dir — the worktree's own path plus `out/build/x64-release/_deps/...` exceeds `MAX_PATH`).
  Configured via `cmake -S <worktree> -B C:/hb/cta5 -G Ninja -DCMAKE_BUILD_TYPE=Release
  -DHE_SDK_RELEASE_SET=OFF -DDEPLOY_DIR=<worktree>/out/deploy` after `vcvars64.bat`. HEAD `d09e65ca`.
  - **he_tests benches:** `HE_ANIM_BENCH=1 ./tests/he_tests.exe --no-skip -tc="*tickAnimation per
    frame over the crowd size*"` and `./tests/he_tests.exe --no-skip -tc="*Foliage extract bench*"`.
  - **Game captures:** scenes via `scripts/perf/gen_autoland_scene.py --extent 512 --density 4` and
    `scripts/perf/gen_reference_world.py --count 10000 --extent 400`; game dirs via a local copy of
    `scripts/perf/make_land_game_dir.py` with its `cp -cR` (macOS-only) replaced by
    `shutil.copytree` — **the checked-in script does not run on Windows as-is**, worth fixing in a
    later step. `config.json` in the game dir's working directory:
    `{"RHI": 3, "CustomConfig": [{"Key": "GameBackend", "Value": "D3D12"}]}` — the editor's top-level
    `"RHI"` field does **not** select the game's backend by itself; `GameApplication` reads the
    `"GameBackend"` string out of `CustomConfig` (`GameApplication.cpp:516-544`). Ran with
    `HE_HIDDEN_WINDOW=1 HE_BACKGROUND_FPS=0 HE_PROFILE_COUNTERS=0 HE_NET_LOOPBACK_ONLY=1`, `APPDATA`
    pointed at a scratch dir, two capture windows per scene: `HE_PROFILE_WARMUP=120
    HE_PROFILE_CAPTURE=250` (steady state) and `HE_PROFILE_WARMUP=1 HE_PROFILE_CAPTURE=5` (catches
    the one-time scatter/terrain-build cost, which falls before frame 120 otherwise). Parsed with
    `scripts/perf/dump_scope_p50.py <dump> <scope...>`.
  - `hive_devices` showed 1.4-1.5% CPU / this bee alone on NN-WS03 before every run — no other bee's
    build or editor running. **One run per scene/window, no noise floor measured** (Schritt 1/2 did
    3 interleaved runs with medians; this step did not repeat that — the numbers above are single
    samples, not medians).
- **Measured vs. estimated:** items 1 (extract share), 2, 3 (scatter number), 9, 10 (chunk-build
  number) are fresh D3D12/Windows game captures; item 11 (animation) and the item-1 cross-check are
  fresh and backend-independent he_tests benches. Items 4 and 8 are Mac/Metal numbers from
  Schritt 1/2, not re-measured (4's candidate was already tried and rejected; 8's own conclusion —
  "nothing to gain" — did not need a Windows re-run to re-confirm, since the code read found
  nothing new). Items 5/6/7 are explicitly out of scope per this step's brief. The `levelOf`
  candidate (item 1) and the TRS/LOD-reuse candidates (items 3/10) are code-read findings (E), not
  measured in isolation — only the surrounding step's total time is measured.
- **Open:** a cells game dir (`scripts/perf/make_cell_game_dir.py`) for a fresh `CellStreaming`
  number was not built this turn; physics and audio were not touched (not in the original 8, not
  named by this step's brief either); `FoliageStraddlers`/`FoliageEmitClusters`/`FoliageScatterLayer`
  were only caught by using two different warmup windows on the same scene — a single capture
  window will miss either the steady-state numbers or the one-time ones, worth remembering for the
  next capture.
- **Decisions taken:** did not re-propose audit item 2 (pre-resolved bounds table) or re-propose the
  SkyNoise3D Worley rewrite — both already resolved/rejected by earlier work, now the first is also
  confirmed by a fresh number; corrected the Schritt-5-draft's own FoliageSystem TRS estimate after
  reading the vendored glm source (`_deps/glm-src/glm/ext/matrix_transform.inl`) — the original
  "≈192 multiplies" guess was wrong, glm's helpers are already specialized; did not touch
  `CellStreamer`'s body-fit scan — bounded, not worth the risk.
- **Decisions suggested, not taken:** the three remaining findings (#1, #10, #3, in that priority
  order per §3) and fixing `make_land_game_dir.py`'s `cp -cR` for Windows use (small, not engine
  code, but blocks the next Windows bee from this exact workflow without a local patch).
- **Traps:** the worktree's own path is long enough that `out/build/x64-release` hits `MAX_PATH` on
  `nlohmann_json`'s FetchContent subbuild — use a short `-B` dir outside the worktree, not the
  preset; `he_tests` without a scratch `APPDATA` rewrites the real editor config; `HorizonGame.exe`
  is a GUI app with no console, so redirecting its stdout captures nothing — read
  `<gamedir>/HorizonEngine.log` instead; a scatter/terrain-build-sized one-time cost needs its own
  `HE_PROFILE_WARMUP=1` capture, a steady-state window never sees it; `hive_devices` before a timing
  run is the Windows equivalent of `pgrep HorizonEditor` — check it, not just once.
- **Next steps:** (1) a `CellStreaming` number via `make_cell_game_dir.py` on D3D12; (2) if #1
  (`levelOf`) or #10 (LOD reuse) are taken up, re-run the same captures as a before/after, the
  harness above is already built; (3) fix `make_land_game_dir.py` for Windows (`shutil.copytree`
  instead of `cp -cR`) as a small, separate, non-topic patch.
