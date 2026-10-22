# Auto landscape 2.0, step 3: texture variants per layer (Thema 180)

Implements `docs/auto-landscape-material-2-plan.md` §5.1 and §6. **Step 2 (BCn) is `failed` on the
board, so everything here is RGBA8; no BCn format, encoder or upload guard was touched.**

## What it does

* Array layout `depth = 4 * V + 1`: layer g's variant v is slice `g * V + v` (Grass, Dirt, Rock,
  Snow), WetGround last. `V = textureSize(array, 0).z / 4` in the shader, so the same material
  renders the 5-slice placeholder arrays (V = 1, exactly as before) and a 13-slice set (V = 3).
* New appended input pin **Groups** on `TextureArrayBombSample`, `NormalMapArrayBombSample`
  (pin 3) and on `TextureArraySample`, `NormalMapArraySample` (pin 2). Unwired = byte-identical
  GLSL to the build before (test pins an FNV hash taken at e6d4920d).
* Wired (auto material: constant 4), each of the three hexes of a bombed read draws its own
  variant from the free low 8 bits of its `heBombHash`: `slice = clamp(group * V +
  ((h.x & 255u) * uint(V)) >> 8u, 0, depth - 1)`, all integers. The new helper `heBombGridV`
  returns the three variant numbers beside the taps; no texture read is added (36 hex reads stay
  36). Albedo/Normal/Mask of one layer share the grid (same seed) and so the same variant.
* The plain (unbombed) array nodes read variant 0 of the group (`slice = group * V`): that is the
  negative control, and the reason Groups is on the plain nodes too (otherwise V = 3 would read
  grass variant 2 for "Rock").
* `landscape_tex_gen --variants N` (synthetic variants, `T_Landscape_<Layer>_<Map>_V<k>.hasset`,
  UUIDs `0x400 + 0x30 + ((k-2)*5 + layer)*3 + map`) and `--pack` with `<Layer>_<Map>_v2.png` ...;
  the array UUID still derives from the LAYER count (5), not the slice count. V = 1 output is
  byte-identical to the old tool (18/18 files `cmp`-equal against a tool built from e6d4920d).
* `ContentManager::resolveTextureRef` logs once when the three auto arrays differ in depth.
* `M_AutoLandscape.hasset` regenerated (`landscape_tex_gen <dir> --material`, 201 nodes).

## Evidence (this Mac, Apple M5)

| Check | Result |
|---|---|
| Groups = 0 text | byte-identical (FNV-1a pin + `cmp` of full dumps, bomb and plain graph, GLSL and G-buffer) |
| V = 1 images, real 2K arrays, `cap158auto.sh` `1`/`nobomb`, top-down + oblique | before vs after, Metal and OpenGL: mean abs diff 0.000, max 1/255 on one frame, 0.000 % pixels > 8 |
| V = 3 images (13 synthetic slices, 128²) | Metal and OpenGL show three variants per layer; snow region classified warm / neutral / cool = 27 213 / 28 841 / 26 746 px (thirds), Metal and GL agree on every sampled pixel |
| Negative control (`nobomb`, V = 3) | variant 0 only, standard checker, no tints |
| Draw calls | 4 before, 4 after, 4 with V = 3 (this is step 5's baseline) |
| `he_tests` | new `test_auto_landscape_variants` (8 cases: byte identity, text of the grouped nodes, 36 reads, chi-square of the variant over 10^5 cells per seed, layer independence, slice mapping, clamp, array assembly); `test_material_graph` 104/104 (one assertion updated: auto graph now calls `heBombGridV`), `test_culling`, `test_engine_materials`, `test_project_exporter`, `test_landscape_reload`, `test_contentmanager` green |
| Cross-translation | all six backends (Metal, GLSL 4.10/ES 3.00/4.30, HLSL, SPIR-V) compile for the grouped graphs and the auto material; integer `& 255u` / `>> 8u` survive in HLSL and MSL |
| HLSL offline | `glslangValidator -D -V -e main -S frag --hlsl-iomap --auto-map-bindings` on the auto material's generated HLSL: rc 0; a deliberately broken identifier: 4 errors (negative control) |
| FXC SM 5.0 | register-range and D3D12 root-signature tests green; the FXC compile test itself runs on Windows only |

The visible hex mosaic in the V = 3 snow image is the **tint contrast of the synthetic variants**
(1.3/0.9/0.65 and so on, flipped checker phase), deliberately exaggerated; it is not the white-array
"hexagon" fault (the textures are real content).

## Handoff

- **Topic / step:** 180, step 3 of 9
- **Branch / commit:** `claude/auto-landscape-material-2-0-alle-backends-bcn-instancing-audit` (see `git log`; pushed: see the result post)
- **Done:** variants per layer in the auto material on all five backends' shared GLSL source, packer support, tests, Metal/OpenGL images.
- **Files touched:** `MaterialGraph.cpp` (pin rows ~:240-260, `bombGrid`/`variantCount` ~:655, array emitters ~:900-1010, `heBombGridV` helper in the helper block), `AutoLandscapeMaterial.h` (V helpers), `AutoLandscapeMaterial.cpp` (Groups const, three connects), `LandscapeTexGen/main.cpp` (`--variants`, `_vN` pack), `ContentManager.cpp` (`noteLandscapeArrayDepth`), `tests/test_auto_landscape_variants.cpp`, `M_AutoLandscape.hasset`.
- **Verified:** see the table above.
- **Not verified:** D3D11 / D3D12 / Vulkan **images** (open: CI `vulkan-lavapipe`, Windows WARP tests, NN-WS03 hardware smoke). `VulkanRenderer.cpp` was not changed, so the MoltenVK `-fsyntax-only` run (rc 0, negative control without the MoltenVK include fails on `vulkan/vulkan.h`) proves only that the headers still parse; it is not evidence for this step. D3D renderers do not compile on the Mac. Real Poly Haven variants (V = 3 from photographs) were not packed.
- **Decisions taken:** Groups on all four array nodes (plan §6.3 named two) so the unbombed control reads variant 0 of each layer; variant grid is a separate helper (`heBombGridV`) so the old helper text stays byte-identical; depth-mismatch log sits in `resolveTextureRef` (one place for five backends), path references only, a baked-UUID ref in an exported pak does not reach it; no "Variation Amount/Scale" parameters (the plan's optional tint was not needed, 13/16 params still).
- **Decisions suggested, not taken:** none.
- **Pending (step 7 or later):** stale pin tables in comments/docs (`MaterialGraph.h` ~:194-195, `docs/auto-landscape-material-textures.md` §8 table ~:675); packing real V = 3 arrays (§12 Poly Haven via `stage_polyhaven.py`, names `<Layer>_<Map>_v2.png`) and getting them to the EngineContent server (not git, doc §18); BCn arrays when step 2 is redone (the shader needs no change: depth is read from the texture).
- **Traps:** `out/deploy/Editor/EngineContent/Textures/Landscape` holds the REAL 2K arrays, not the 128² placeholders (placeholders are not in git any more): swap the three `_Array` files for `landscape_tex_gen <dir> --variants 3` output to see V = 3, then copy them back (backup first). The deploy editor binary from before the merge dies on a missing symbol: build `HorizonEditor` first. Metal vs OpenGL differ by mean 2.0 on this scene both before and after (pre-existing).
- **Next steps:** 1. step 4/5 use the same arrays untouched; 2. redo step 2 (BCn) on top of this commit; 3. CI `vulkan-lavapipe` image test with a 13-slice array.
