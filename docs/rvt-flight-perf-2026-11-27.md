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
- That alone left p95 at ~47 ms (31-35 of 361 frames > 33 ms). The cause, found with a still camera (start and end both
  16.8 ms GPU) and `HE_RVT=0` (G-buffer 26 ms flat): the tile RVT volume (4.2 km, 2048 pages) waited for a successor
  filled from scratch (~600 pages) and swapped once, 200 frames late; the ground ahead outside the old square ran the
  whole landscape material (G-buffer 14 → 25 ms). Fixed, step 19 (commits `288b6b03d`, `44f063224`, `d1680139b`):
  - `RvtManager::moveVolume`: the square moves with the camera (quarter-edge grid = whole pages), pages that land on the
    new grid keep slot + picture under a NEW volume id (stale feedback ignored); edge ring and non-aligned mips dropped.
  - `refreshSubtree` under a wide page: existing pages over a plain parent copy (was a hash lookup per mip-0 entry,
    25-40 ms for the top page). `setWidePageMeters(1000)`: pages ≥ 1 km one a frame. Move ticks now 9-12 ms.
  - Metal table upload: staging buffer per ring entry + remap on the workers (5.9 → 2.0 ms a frame; the table changed every
    frame in flight). `RvtPageRendersInFlight` gate (`HE_RVT_MAX_IN_FLIGHT`, 6): no new pages while 6+ layer renders queue.
  - TAA: a raw capture destroyed the TAA/MetalFX targets (its AA reads Off): 497 of 720 resolves without history → 3.
- **Result**, tile flight `--stats-only`: p50 10-11 ms, p95 17-19 ms, 0-1 of 361 frames > 33 ms (was p95 47, 31-35). Pictures
  (`--keep-frames`): no frame-to-frame jump at the 3 moves. ctest 411/411; `landscape_gpu_check` metal/gl/vulkan exit 0.
- **Not fixed: near ground blurs in fast flight.** Near-ground sharpness (`rvt-flight-perf-2026-11-27/sharp.py`, mean |dx| bottom third; `framediff.py` for jumps) 2.0 still
  (2.57 full material) → 0.26 after 140 frames at 300 m/s in BOTH builds; late in the flight now ~0.1 (the old build
  showed 2.5 there only because it ran the full material). 16/32 pages a frame: 0.30. The fine pages do not keep up.
- Tried and dropped: Metal queue of 256 command buffers (stall gone, frames over 31 → 123: lost back-pressure); a page-size
  target set (neutral); a LOD camera for wide pages (neutral). D3D/GL/Vulkan: scene-side changes unverified there (step 18).

**Next steps**

1. RVT page throughput in fast flight: what the near pages wait for (ranking of bare vs. near pages, the distance clamp
   `band0` in `tileGroundViews`, pool size vs. ~1500 wanted). Sharpness against `sharp.py` on `fly_tiles.json`.
2. Merge `claude/landscape-migration` into `claude/landscape-one-system` after the bee's step 6.
