# Tile ground on D3D11 / D3D12 (topic 355, step 18, NN-WS03, 27.11.)

Step 18: on Windows, run the slot-table alias fix, measure the RVT table upload, the far image from the material. All three
parts are done here.

## Handoff

**Done** (branch `claude/landscape-one-system`):

1. **Alias fix runs on D3D.** The HLSL fixed blend (`LandscapeShaderSource.h:816`) and the material head
   (`MaterialShaderLibrary.cpp:1717`, `heLsFind`) reject a tile half a table window from the camera's tile on D3D11 and D3D12
   (`cameraRel` comes from `D3D_Shared/LandscapeD3D.h:70`). No code change was needed.
2. **RVT page table upload, cheaper.** The manager sets its table once a frame while pages arrive; a 100 km volume's is 2048²
   entries plus mips (22 MB), rewritten slot by slot on every call.
   - D3D11 `uploadRvtTable` (`D3D11Renderer.cpp` ~10468): each slot looked up once (slot map), the rewrite buffer kept in
     the table (`RvtTable::mapped`) instead of a fresh 16 MB vector per call.
   - D3D12 `uploadRvtTable12` (`D3D12Renderer.cpp` ~15290): the same slot map, entries written straight into the upload
     buffer (no intermediate vector + row memcpy). Still synchronous, still one upload buffer per call (see Fallen).
3. **Far image from the material on D3D** (Metal's `547bfe380` + `d2d547ac2`):
   - Drawn: the HLSL landscape variant gets `HE_LS_FAR` (`MaterialShaderLibrary.cpp` fragmentVariant, `farImage` for HLSL);
     `heLsFarImage` (binding 62) → **t56, s0**: s0 is the variant's one live sampler (heLandscapeWeights, s0's live user in
     every other material, is replaced in it). D3D12: `kLandscapeSrvCount` 9 → 10 (`D3D12MaterialRootSignature.h`), the static
     s0 is linear + mips + clamp already; `D3D12Landscape::writeMaterialViews` writes the far view (or a null one) tenth.
     D3D11: `D3D11Landscape::drawPatches` binds the far SRV on t56 and its own far sampler on s0, puts the material's s0 back.
     Both `drawPatches(…, farImage)` set `uTile.w`.
   - Baked: `Capabilities::landscapeGroundImage` on D3D11/D3D12 (= `landscapeRvt`). `drawGroundMaterial`: rvtTileGround with
     no slot = the ground image: `rvtMaterial` on the whole ground, no RVT consumer patches, no far image (also not in the
     fixed blend: `Shading::noFarImage`).

**Verified** (Release, RTX 4070; test world `make_world.py` here: HEProject's scene with only terrain/sky/weather, terrain
stretched to 100 km with far_testworld's mountain/F/rock/snow; tiles via the baseline's `tools/migrate.cpp`, 196 × 196)

- Table cost per frame, temporary timers (removed), 12 s flight `flight100k.json`, RVT on, old landscape:
  D3D11 6.5 → 5.2 ms (copy ~1 ms of it), D3D12 10.5 → 6.7 ms. Pages: D3D11 1.5 ms, D3D12 9-10 ms a frame (24 layers).
- Frame work p50 (`he_flythrough.py --stats-only`, 3 reps after, 3-7 before): D3D11 21.3-21.7 vs 21.2-23.6 ms (within noise);
  D3D12 28.6-29.1 vs 29.6-34.5 ms, p95 36-40 vs 40-49. RVT off for scale: D3D11 10.9, D3D12 2.8-4.0 ms.
- Pictures unchanged: ground below the horizon (`grounddiff.py`), frames 100/200/330: D3D11 before vs after 0.00 levels;
  D3D12 after vs D3D11 0.10-0.26 levels, 0 px > 30. (The sky differs between runs: clouds drift with the wall clock.)
- Alias A/B on the tile world, camera on the mountain at (75 000, 3 500, 25 000) looking west (`stillMountain.json`): with
  the test switched off the near rock/snow discs repeat as ellipses at the horizon (`t100m-D3D11-alias-off.jpg`), with it
  they are gone (`t100m-D3D11-alias-fix.jpg`); 5 983 px differ on D3D11 and on D3D12 alike.
- `landscape_gpu_check --only d3d11|d3d12` exit 0; he_tests 6 620 passed, 0 failed (parts 1-2, 6f5e03483).
- Far image (part 3), 40 km tile world (`make_world.py … 40000`, migrated: 79 × 79 tiles), RVT on, Manual bake pressed
  by `HE_LANDSCAPE_FAR_BAKE_NOW=1` (`farbake.json`): "100 region(s) painted from the material" in 91.8 s (D3D11) and
  94.6 s (D3D12; Metal 91 s). The decoded `.far` (`far_to_png.py`, its `zstd` call swapped for Python's `compression.zstd`)
  has the mountain with rock ring and snow north-east and the hill south-west, not mirrored (`far-baked-d3d11.jpg`);
  D3D11 and D3D12 bakes differ by at most 1 level. Drawn, from (38 000, 3 000, 38 000): `HE_LS_FAR` 0/1 changes 92 241
  ground px by > 2 levels, all beyond the atlas edge (`far-ab-d3d11.jpg`: off, on, difference × 8); D3D11 vs D3D12 with
  it 0.13 levels. `test_material_graph`'s D3D register case checks t56/s0.

**Not verified:** RVT on the tile ground in flight on D3D (all flights above are the old landscape: the tile RVT
thrash of step 15 would dominate); D3D11 / D3D12 under the debug layer; the far image's weather snow on D3D (Snow Cover
> 0: same GLSL as Metal, not rendered here); Automatic bake mode on D3D; GPU cost of the far branch.

**Fallen**

- D3D12: a KEPT, persistently mapped 22 MB upload buffer (one, or one per frame slot with the copy recorded into the
  frame's own list and no waits) made the RVT driver's page renders 31 instead of 9.5 ms a frame in about half the runs
  (bimodal: p50 ~23 or ~54 ms). A fresh buffer per call: 5 of 5 runs normal. Not understood (residency of a large UPLOAD
  heap?); the slow runs sat at P0, so it is not the GPU clocking down.
- `Copy-Item` keeps the source's mtime: copying a saved .cpp back over a checked-out one leaves ninja "up to date" and the
  next A/B measures the old code. Touch the file.
- `he_flythrough.py` defaults `--editor` to `<repo>/out/deploy`; pass the isolated tree's editor. No ffmpeg on NN-WS03.

**Decisions:** D3D12 keeps its synchronous table upload (the faster deferred form above is a regression in half the runs).

**Next steps**

1. Vulkan / OpenGL: the same far image from the material (step 16); the D3D shape above is the template (a free register
   and a live sampler in the material layout, `drawPatches(farImage)`, `landscapeGroundImage`).
2. D3D12 page renders (9-10 ms a frame vs 1.5 on D3D11): every page bracket opens with `rvtStillEnter` →
   `createViewportRT` → a full flush, 50-60 a second.
