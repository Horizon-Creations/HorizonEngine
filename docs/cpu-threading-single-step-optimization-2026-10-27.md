# CPU threading: per-step algorithmic optimization (Thema 238, Schritt 5)

27.10.2026, NN-WS03 (Windows, AMD Ryzen 9 9950X, 16C/32T), **D3D11/D3D12 build, no Metal on this
machine** — a deviation from the brief's "single Metal backend", posted to the topic as a warning
before this doc was written. **No engine code changed.** New: `he_tests` Release build in
`C:/hb/cta5` (short-path override of `out/build/x64-release`, MAX_PATH), two raw benches in
`docs/perf-audit/raw-cpu-threading/win-*-2026-10-27.{csv,stdout.txt}`, this doc.

Builds on `cpu-threading-audit-2026-10-22.md` (Schritt 1, 8 hot spots on Metal/M5) and
`cpu-threading-dependencies-2026-10-24.md` (Schritt 4, task graph). This step looks at each of
those 8 items **again, one at a time, for a LOCAL (non-parallel) shortening**: algorithm,
data structure, unneeded work, early exit, caching/precompute — not "can it run on the pool"
(that is Schritt 2/6).

## 1. What changed under this audit since Schritt 1/2 (read before trusting any old number)

The branch's `HEAD` (`d09e65ca`) is release/0.7.0 plus this topic's own commits, and release/0.7.0
has absorbed a **separate** topic's work (`mac-frame-levers`, "GPU-performance-audit" successor,
PRs #186/#194/#198/#199) that touched the same files this audit's item 2 is about:
`RenderExtractor.cpp` (`c38943ad` "extractMeshes runs wide: filter, count, build in place,
multi-section fix-up serial", `498de938` "Extract and frame CPU hotspots: in-place EntityData,
component pools once, mesh memo, wide RefineBounds, counting-sort sortByMesh", `56b4fbec` "Wide
transform scan, material scalars and shadow scene box"). **Audit item 2 (pre-resolved
bounds/material table so the gather does not call `ensureResident` per entity) is already done**
— read at `RenderExtractor.cpp:349-390` (3 wide phases over 2048-entity blocks: filter, build
in place into `out.objects`' prefix-summed slots, multi-section fix-up serial only where a load
may happen). Schritt 1's 1.4-2.16 ms number for this item (`ExtractFrame`/10k meshes) is **stale**
and must not be compared against a fresh run without re-measuring on this code. I did not re-time
it (out of this step's "no code, no build-for-timing-the-renderer" budget beyond the two he_tests
benches below) — **flagged for Schritt 6 to re-baseline before doing anything else to it.**

`TransformHierarchy::propagateTransforms` (`TransformHierarchy.cpp:450-520`) — named as a
candidate in the Schritt 4 doc ("dirty-only transform propagate") — **is already dirty/subtree
adaptive**: full walk only when the structure epoch moved or `forceFull`, otherwise a scan flags
changed entities and climbs only their subtrees, with a hysteresis (`preferFull`) that switches
back to a full walk when most of the world is flagged. This predates topic 238; nothing to do.

## 2. Table

Confidence: **M** = measured this run (Windows, Release, backend-independent he_tests bench);
**MS** = measured, but by an earlier step/platform (Mac/Metal) and not re-verified here — treat
the ranking as transferable, the absolute ms as not; **E** = estimated from a code read, no run;
**U** = reasoned, unverified.

| # | Step | File:line | Measured | Local candidate | Speedup est. | Effort (1-5) | Risk/pitfall | Conf. |
|---|---|---|---|---|---|---|---|---|
| 1 | `ExtractFoliage` walk — bucket plan + straddler test (memo+pool already landed, Schritt 2) | `FoliageExtract.cpp:457` `planBuckets`, `:488` `runStraddler`, `:561` `emitPlans`, `:790` `extractFoliage` | MS (Mac/Metal, Schritt 2): 0.011-0.014 ms memoized (still camera), 0.17-0.91 ms moving camera, one thread 0.343-0.915 ms. **M (this run, Windows, 524k instances, moving camera):** whole extract one-thread **0.488 ms**, pool **0.244 ms** median of 600 (`win-foliage-extract-bench-2026-10-27.csv`) | `runStraddler` (`:488-557`) allocates a fresh `std::vector<uint8_t> levelOf(b.count, kUnknown)` (`:529`) **every call**, i.e. every straddler bucket of every frame the camera moves — `r.mats`/`r.clusters` are already reused via `StraddlerBuffers::results` (`:570-583`, cleared not freed) but `levelOf` is a plain local, so it heap-allocs+frees under a moving camera at the rate buckets are re-tested. Move it into `StraddlerResult` (resize-not-reallocate, same pattern as `mats`) | small (allocator overhead only, bucket counts are tens to low hundreds of plants each) — not separately measured, no allocation counter in this run | 2 | `levelOf` is sized to `b.count` which varies per bucket; reusing it means `assign`/clear to the new size each call instead of a fresh vector, cheap but must not leave stale bytes past the new `b.count` for the next, larger bucket | E |
| 2 | `extractMeshes` gather (ensureResident-per-entity) | `RenderExtractor.cpp:271-452` | MS, **stale** — Schritt 1 said 39% of WORK at 10k meshes (`ExtractFrame` 2.16 ms), **but the gather this measured no longer exists**: see §1. Not re-timed here | **Already implemented** by the frame-levers topic (wide filter/build/fix-up, mesh memo, counting-sort `sortByMesh`) — nothing left for Schritt 5 to propose. Re-baseline in Schritt 6 before judging it done | n/a (done, unverified by number) | n/a | re-baseline before building on this item; Schritt 1's number must not be quoted again without a fresh run | U |
| 3 | `FoliageSystem::scatterLayer` (per-instance RNG + placement rules + pose) | `FoliageSystem.cpp:185`, hot loop `:249-299` | MS (Mac/Metal, Schritt 2, 524k instances, one-time): **55.8/56.3/50.2 ms before → 31.9/31.4/30.7 ms after** (pool landed) | Per instance (`:291-293`) builds the pose via `glm::translate(mat4(1)) * glm::rotate(...) * glm::scale(...)` — three 4x4 matrix multiplies (≈ 192 mul+144 add) for a rotate-about-Y + uniform-scale + translate that a direct construction writes in ~20 multiplies (two sin/cos, 4 scaled entries, translation column) | ~10-20% of the per-instance cost (the hash/lookup calls beside it do not shrink) — not isolated/measured separately | 2 | **The existing bit-identical tests compare floats bit for bit** (`test_foliage_parallel.cpp`, `foliage_pixel_ab.py`, the Schritt 2 picture A/B). A hand-rolled TRS is mathematically equivalent but rounds differently in the last bit(s) than three chained `glm` multiplies — this is not a drop-in, it needs the bit-identical baselines re-recorded (not just re-passed) and a reviewer who accepts that the "identical" contract moves | E |
| 4 | `SkyNoise3D::BuildSkyNoise3D` | `SkyNoise3D.cpp:10-83` | MS (Mac, Schritt 1/2): 845 CPU-ms / 200 ms wall at n=256, already thread-fanned (`:75-81`) | **Already tried and reverted** (Schritt 2): a cheaper Worley (cell-skip distance bound) was *slower* (1070 vs 845 CPU-ms). The 48³ jitter precompute (`:24-34`, comment cites "~453M at n=256") is itself a prior fix (perf audit B7, pre-238). Nothing further found by this read; a disk cache remains a product decision (where does a player's machine keep 64 MiB), not a tuning step | n/a | n/a | do not re-propose the Worley rewrite, it was measured slower | E |
| 5 | `UpdateSkyEnvCube` rebake | `MetalRenderer.mm:13887` (D3D11 `:4712`, D3D12 `:9227`, Vulkan `:3055`, GL `:76`) | — | **Out of scope** (GPU time, excluded by this step's brief) | — | — | — | — |
| 6 | Editor `warmupWorldMaterials` / cold-start load | `EditorApplication.cpp:2288` | — | **Out of scope** (editor-only, excluded by this step's brief) | — | — | — | — |
| 7 | `ContentManager::parseAndRegisterAsset` over the pak/async path | `ContentManager.cpp:169` (scope at `:175`, added Schritt 2) | — | **Out of scope** (async/pak path, explicitly excluded — "out of scope per handoff run 1155") | — | — | — | — |
| 8 | `SceneJsonParse` fan-out / `CellStreamer` slice build | `SceneJsonParse.cpp:252`, `CellStreamer.cpp:551-634` | MS (Mac, Schritt 1): parse 102 ms parallel / build 75 ms serial at 10k entities; `CellStreaming` main-thread scope 120 ms over 500 frames (p50 0.015 ms) | Read `CellStreamer.cpp:551-634` again for this step: the build is already nearest-first, slice-by-slice, time-budgeted (`overBudget()`, `:559-562`), and a cell that is mid-build is finished rather than cut — matches Schritt 1's own conclusion ("build stays on the main thread within the 4 ms budget... nothing to gain here"). The one thing *not* already tight: `bodiesFit` deferral does a `std::any_of` scan over all of `m_pending` (`:583-585`) on every deferred cell check, which is O(pending) instead of O(1), but `m_pending` is bounded by in-flight cells (tens, not thousands) — not worth prioritising | ~0 | n/a | none found worth the risk of touching a correctness-sensitive streaming budget | E |
| 9 (new) | `SceneSystems::tickAnimation` / `AnimationSystem::update` — **never measured before this step** (Schritt 1 had no animated scene) | `SceneSystems.cpp:230`, `AnimationEval.cpp:27` `findBracket`, `:55` `applyChannel` | **M (this run, Windows, he_tests `HE_ANIM_BENCH=1`)**: linear in crowd size, **3.85-4.00 µs/character** (65 joints, 31-key full-body clip) at every size tried (1, 50, 200, 500, 1000) — `win-anim-bench-2026-10-27.csv`. 1000 characters = **3.85 ms** of serial main-thread work | `findBracket` already runs an **adaptive** linear-vs-binary-search keyframe lookup (`kBracketBinaryFrom = 64`, comment cites an earlier audit, `docs/audit-scene-systeme-2026-10-18.md`, that measured the crossover on the M5) — this is a pre-238 optimization, not something this step found newly broken. No further single-step shortening found by this read (no redundant work, no missing early-out: `sampleClip` already skips a channel whose `jointIndex` is out of range, `:134`) | 0 (nothing found) | n/a | the *real* lever here is parallelization (one thread per skeleton group, or per-character jobs), which is Schritt 2/6 scope, not this step's | M |
| 10 (new) | `TerrainSystem::buildChunk` mesh generation | `TerrainSystem.cpp:318-394`, `TerrainMeshGenerator.cpp:378` `generateTerrainChunkMesh` | MS (Mac, Schritt 1): 7.7-11 ms for 16 chunks (one-time/streaming cost, not per-frame) | Each of `g.numLODs` levels (65, 33, 17, 9, … verts/side, `TerrainSystem.cpp:346`) independently resamples the **same** height field via `sampleField`/`sampleFieldNormal` (`TerrainMeshGenerator.cpp:384-388`) — but LOD(k+1)'s grid is an exact stride-2 subset of LOD k's (both walk the same `u0..u1` range with `verts = lod0Cells>>k + 1`). Downsampling LOD0's already-computed heights/normals into the coarser levels instead of resampling the field would remove most of the redundant finite-difference normal samples | ~20-30% of `buildChunk`'s CPU time (not isolated/measured; geometric falloff means the coarser levels are already cheap: 65²+33²+17²+9² ≈ 1.35× a single LOD0 pass, so the ceiling on this one is modest) | 3 | Touches shared mesh-gen code used by every terrain in every scene; the stride-2 grid-coincidence assumption holds for every current call site (`g.lod0Cells>>k+1`) but would silently stop applying if a future caller passed an unrelated `vertsPerSide` — needs a guard that falls back to full resampling. This is a **streaming/load-time** cost, not a per-frame one — lower priority than anything that runs every frame | E |

## 3. Priority (effort vs. measured/estimated benefit, local-only)

1. **#9 Animation — no action, but now has a number.** Fills the gap Schritt 1/4 flagged ("no
   animated scene measured"). 3.85 ms serial at 1000 characters is real budget if a scene has a
   crowd; the fix is parallelization (Schritt 2/6), not shortening — the algorithm is already tight.
2. **#3 FoliageSystem TRS construction** — cheapest local win on the list (effort 2), but the
   bit-identical test contract means it is not silent: whoever does it must re-record the
   baselines, not just watch them pass.
3. **#1 `levelOf` scratch allocation** — small, safe (same reuse pattern the rest of the straddler
   path already uses), only pays while the camera moves.
4. **#10 TerrainSystem LOD reuse** — real but modest ceiling, touches shared code, streaming-time
   only: lowest priority of the four found.
5. **#2 (re-baseline), #4 (closed), #5/6/7 (out of scope), #8 (closed)** — nothing to do.

## 4. Handoff

- **Topic/step:** Thema 238, Schritt 5 (global step id 1202). Branch
  `claude/cpu-threading-audit-jobsystem-auslastung-daten-vorbereitung-`, this doc + two raw
  benches, no engine code.
- **Reproducibility:** `he_tests` Release, Ninja/MSVC, `C:/hb/cta5` (short-path build dir — the
  worktree's own path plus `out/build/x64-release/_deps/...` exceeds `MAX_PATH`), configured via
  `cmake -S <worktree> -B C:/hb/cta5 -G Ninja -DCMAKE_BUILD_TYPE=Release -DHE_SDK_RELEASE_SET=OFF
  -DDEPLOY_DIR=<worktree>/out/deploy` after `vcvars64.bat`. HEAD `d09e65ca`. Commands:
  `HE_ANIM_BENCH=1 ./tests/he_tests.exe --no-skip -tc="*tickAnimation per frame over the crowd
  size*"` and `./tests/he_tests.exe --no-skip -tc="*Foliage extract bench*"`. `APPDATA` pointed at
  a scratch dir (`C:/hb/cta5_appdata`) and `HE_NET_LOOPBACK_ONLY=1` set before every run, per the
  `he_tests` lesson (it otherwise touches the real `%APPDATA%` config). `hive_devices` showed
  1.4% CPU / this bee alone on NN-WS03 at run time — no other bee's build or editor running.
- **Measured vs. estimated:** #9 (animation) and the foliage-extract cross-check are fresh,
  measured, Windows, backend-independent (the bench calls `RenderExtractor::extract`/
  `SceneSystems::tickAnimation` directly, no renderer attached). Everything else in the table is
  either a Mac/Metal number from Schritt 1/2 (marked MS, not re-verified) or a code read with no
  run (marked E). No D3D11/D3D12/Vulkan **renderer** capture was taken — building and driving
  `HorizonGame`/`HorizonEditor` with a real scene (to get `RenderExtractor::extract`, `Terrain`,
  `CellStreaming` scope numbers via `HE_PROFILE_CAPTURE` + `scripts/perf/dump_scope_p50.py`) did
  not fit this turn after the Release engine build; the scope names exist and are listed in §1/§2
  for whoever does this next (`HE_PROFILE_SCOPE_N` inventory: `Terrain`, `TerrainTess`,
  `TerrainSeams`, `Animation`, `AnimationBlend`, `CellStreaming` (`GameApplication.cpp:1617`,
  `EditorApplication.cpp:4067`), `Extract::meshes`, `Extract::foliage`, `RefineBounds`,
  `MaterialScalars`, `SortObjects` (the last three exist per-backend, including D3D11/D3D12)).
- **Open:** a real D3D11/D3D12 game/editor capture (scenes, `make_land_game_dir.py` +
  `gen_reference_world.py`, `HE_PROFILE_CAPTURE=250 HE_PROFILE_WARMUP=120`, `dump_scope_p50.py`)
  would replace the MS rows with fresh Windows numbers and is the natural continuation; physics
  and audio were not touched (not in the original 8, not named by this step's brief either).
- **Decisions taken:** did not re-propose audit item 2 (pre-resolved bounds table) — it is already
  implemented by a different topic's commits; did not re-propose the SkyNoise3D Worley rewrite —
  already tried and measured slower in Schritt 2; did not touch `CellStreamer`'s body-fit scan —
  bounded, not worth the risk.
- **Decisions suggested, not taken:** the three new findings (#1, #3, #10) and the re-baseline of
  #2, in the priority order of §3.
- **Traps:** the worktree's own path is long enough that `out/build/x64-release` hits `MAX_PATH`
  on `nlohmann_json`'s FetchContent subbuild — use a short `-B` dir outside the worktree, not the
  preset; `he_tests` without a scratch `APPDATA` rewrites the real editor config; `hive_devices`
  before a timing run is the Windows equivalent of `pgrep HorizonEditor` — check it, not just once.
- **Next steps:** (1) D3D11 game/editor capture on `s512_x4`/`ref10k`-equivalent scenes for fresh
  Extract/Terrain/CellStreaming numbers; (2) Schritt 6 re-baselines audit item 2 before deciding
  anything about it; (3) if #3 (TRS construction) is taken up, re-record the bit-identical
  baselines as part of the same change, not after.
