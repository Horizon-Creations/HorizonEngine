# RVT: frame rate while the camera moves (owner's report on Catania, 27.11.)

## Handoff

**Report:** moving through Catania the frame rate drops; black lines between landscape tiles; the far image does not
show. The owner ran release/0.8.0 at f83f209f5 with Catania still on the OLD landscape (one 100 km TerrainComponent,
flat, one layer), not the tile world.

**Found and fixed** (branch `claude/landscape-one-system`, Metal):

1. **Frame drop = RVT page painting.** Owner's own log: 4.6 ms per page, up to 8 a frame (37 ms). Sample of the editor
   in flight: `MetalRenderer::UploadRvtTable`. A 100 km volume's page table is 2048² + mips (21 MB); it was remapped and
   written with `replaceRegion` (CPU tiling) inside every page render (`UpdateRvtCold` step 4 ran in each `EncodeFrame`),
   again in `SetRvtPageTable`, into a new texture + buffer each time.
   - `494725791`: step 4 skips page frames (`rvtSlot >= 0` / `rvtTileGround`); `SetRvtPageTable` marks dirty, one upload a
     real frame; entries go through a shared buffer + blit; raw captures skip CSM, cloud shadow, sky LUT, fog volume,
     cloud pre-pass; captures no longer step GPU particles.
   - `12e7ebd28`: ring of 4 table textures + one reused staging buffer (no 21 MB allocations per upload).
2. Flight tool: frame-work stats in the log, `--stats-only` (no read-back: the editor's own times), config in
   `--cfgdir` kept (copy the owner's `config.json` there to fly with their settings).

**Verified** (copy of the owner's Catania + their config, `scripts/he_flythrough.py --stats-only`, 4 km at 235 m in 12 s):

| | p50 | p95 | frames > 33 ms |
|---|---|---|---|
| before | 48.8 ms | 57.9 ms | 327 / 361 |
| after | 11.2 ms | 13.4 ms | 0 / 361 |
| RVT off | 6.3 ms | 10.4 ms | 0 / 361 |

Page cost 3.88 → 0.48 ms. Frames 100/200/330 of the recorded flight: mean difference 0.1-0.2 levels. ctest 410/410,
`landscape_gpu_check` metal / gl / vulkan exit 0.

**Not fixed, cause known**

- **Black lines (old landscape only):** dashed dark lines along chunk borders. Gone with GI off, stay with SSAO off: the
  ray-traced GI sun mask at the chunk seams (topic 190's class), here on a FLAT terrain where `stitchSeams` has no step
  to close. The migrated tile Catania shows none from the same view with GI on.
- **Far image (old landscape):** never baked (Manual mode, no `Saved/FarLandscape`). With Automatic: 256 chunks in 5.3 s,
  drawn. The owner presses Landscape ▸ Setup ▸ Bake Far Landscape, or sets Preferences ▸ Far Landscape Bake = Automatic.
- GL / Vulkan / D3D upload their tables their own way: not measured, not changed.

**Tile world in flight, step 15 (branch `claude/landscape-migration`, 27.11.)**

- Pool grows at run time: `RvtLandscape::growPoolIfShort` (8 full ticks with starved pages or feedback > pool: +25 %, or
  feedback + 12.5 %, 64-page steps, cap 2048, 30-tick cooldown) → `RvtManager::growPool` + `IRenderer::GrowRvtPoolSlots`
  (Metal only; refuses past 70 % of `recommendedMaxWorkingSetSize`, then the starved warning). Witnessed in the app on the
  tile Catania: 1024 → 1280 → 1600 pages, no starved warning. Test: `test_rvt_core` "a pool that grew takes the pages…".
- Paint budget: `HE_RVT_PAINT_BUDGET_MS` (default 6) / the recent CPU cost a page, after the first fill. Profiler scopes
  `Rvt::feedback|updateAll|paint|tileTables|tileVolumes|tileWanted`.
- **p95 did not move** (tile flight p50 14.3-14.9, p95 46.6-48.7, 31-35 of 361 frames > 33 ms; before the step ~137). Measured why:
  - with no page painted after the fill: p50 4.9, p95 10.8, 0 over, so the cost is the pages' GPU work;
  - the last ~2 s of the path the REAL frame's GPU climbs 17 → 25-30 ms even without painting (detailed capture: the
    "GBuffer" command buffer 13-16 → 23-27 ms); the old-system flight stays flat at 16-17 ms. With pages on top it passes 33.
  - The 4-page ticks that take 35-40 ms are `[queue commandBuffer]` waiting at Metal's 64-buffer limit. Tried
    `newCommandQueueWithMaxCommandBufferCount:256`: the stall goes, but frames over budget go 31 → 123 (the stall was the
    only GPU back-pressure; the budget sees CPU time and clamps to 8). Reverted. Per-command-buffer GPU times are no budget
    signal either (a page frame reads 0.2 ms, 20-30 ms under overlap).
  - A page frame (136 px) rebuilds the live HDR / LDR / G-buffer targets and the next real frame rebuilds them back (525× in
    12 s). Keeping a separate page-size set measured neutral; not committed.
  - `RenderSceneImage` clears `m_taaHistoryValid`: the viewport loses TAA history on every frame that paints a page.

**Next steps**

1. Bring 494725791 + 12e7ebd28 to release/0.8.0 (the owner runs that branch) — owner's call. Done (894761fd5).
2. Tile-world real-frame GPU at the end of `fly_tiles.json` (51489,235,63781 → 47889,235,62021): a still camera at the end
   key with pages filled first. Near 17 ms → the cost comes from motion (streaming, table/atlas uploads, the 1100-1300
   wanted vs 600-900 resident backlog); near 28 ms → that location (what the G-buffer draws there).
3. Then a GPU-aware paint budget (back-pressure on page command buffers still in flight, not their GPU times).
