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
- **Tile world in flight:** p50 10.9 ms but p95 44 ms: feedback wants ~1350 pages, pool holds 1024, so 8 pages are
  repainted every frame at ~4 ms each (pool full). Pool size / texel size of the tile RVT, not the upload.
- GL / Vulkan / D3D upload their tables their own way: not measured, not changed.

**Next steps**

1. Bring 494725791 + 12e7ebd28 to release/0.8.0 (the owner runs that branch) — owner's call.
2. Tile RVT pool thrash in flight (pool vs. feedback demand).
3. Catania onto the tile world (topic 355 step 9), which also removes the GI seam lines; else a GI-seam fix for the old path.
