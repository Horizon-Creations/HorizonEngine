# Auto landscape 2.0, step 10: BC1 / BC4 / BC5, encoders and upload paths (Thema 180)

Implements `docs/auto-landscape-material-2-plan.md` §7.4 (steps 1 to 5). **Nothing in the
Auto-Landscape material changes its format here**: no `landscape_tex_gen`, no
`M_AutoLandscape.hasset`, no shader code, Metal stays on ASTC.

## What changed

| Area | Change |
|---|---|
| `Assets.h` | `TextureFormat` gets `BC1 = 4`, `BC4 = 5`, `BC5 = 6` (appended, the old values are the TXMI byte on disk). New `textureFormatBlockBytes` (8 for BC1/BC4, 16 for the rest, 0 for RGBA8), `textureLevelBytes`, `textureFormatName`. `textureFormatIsBlock4x4` stays the block test. |
| `TextureArrayBuild` | `textureArraySliceBytes` / `textureArrayOffset` take a format (default RGBA8, so every old caller is unchanged). `textureArrayPayloadValid` accepts any known block format with the right byte sum. |
| `HpakWriter.cpp` | `encodeBc1` (stb_dxt, alpha 0), `encodeBc4` (R), `encodeBc5` (R, G), cook targets 4, 5, 6. `cookTexture` can also cook **texture arrays** when `PackSettings::cookTextureArrays` is set (default off): each slice and level is encoded as it stands (the mips `buildTextureArray` baked are kept), `layers` stays in the TXMI tail. |
| `TextureBlockDecode.*` (new) | CPU decoders for BC1/BC3/BC4/BC5 (`decodeTextureToRGBA8`). Used by the round-trip tests and as the runtime fallback below. BC7/ASTC have none. |
| OpenGL | `glSupportsRgtc()` (core since GL 3.0), `glBlockInternalFormat`, block-compressed arrays through one `glCompressedTexImage3D` per level (all slices gathered, because a null-data allocate is not portable for compressed formats). |
| D3D11 / D3D12 | `DXGI_FORMAT_BC1_UNORM(_SRGB)`, `BC4_UNORM`, `BC5_UNORM`; the "arrays are RGBA8 only" guards are gone (the subresource walk was already generic). |
| Vulkan | `VK_FORMAT_BC1_RGB_*`, `BC4_UNORM_BLOCK`, `BC5_UNORM_BLOCK` behind the existing `vkGetPhysicalDeviceFormatProperties` guard; block arrays allowed; level math via `textureLevelBytes`. |
| Metal | BC1/BC4/BC5 pixel formats next to BC7/BC3 (same `supportsBCTextureCompression` guard), and the array path takes block formats (needed: ASTC arrays now come out of the export cook). ASTC itself is untouched. |
| Fallback | A device that cannot sample BC1/BC3/BC4/BC5 gets an RGBA8 copy (CPU decode) and one log line per format on GL, D3D11, D3D12 and Vulkan. BC7 (Apple GL) and ASTC have no decoder, they still return "unusable" (flat), as before. Metal has no fallback. |

BC4 is sampled as `(r, 0, 0, 1)`, BC5 as `(r, g, 0, 1)`. A normal map in BC5 has **no Z**: the
`.xyz` read of the Normal nodes (`MaterialGraph.cpp:953`) would give z = 0. Not wired anywhere
in this step; §7.3 of the plan (rebuild Z in the node) is the step that has to do it.

## Arrays are cooked only on request

`PackSettings::cookTextureArrays` (default **false**) switches array cooking on in
`cookTexture`; it is part of the pack fingerprint (bit 6, so the fingerprint of every
existing setting is unchanged). With it off, an export ships arrays RGBA8 exactly as
before. It is off because turning it on for the Auto landscape arrays would be a
regression in a shipped game: `giAutoLandscapeSliceMeans` (`GiAutoLandscape.cpp`) reads RGBA8
texels and has no BC7/ASTC decoder, so the GI landscape colours would fall back to their
defaults. Nothing sets the flag yet (the export dialog and `ProjectExporter` leave it false). The step
that does enable it has to carry the five slice means first (compute them at pack time
from the RGBA8 source, or decode a small level at load).

## Verified (this Mac, M5, Release `out/build/macos-release`)

- `make he_tests` and `make HorizonEditor` build (Metal and OpenGL renderers compile with the change).
- `he_tests --source-file=*test_texture_bcn.cpp`: 12 cases, 343 assertions, all pass. They cover
  the enum values, block-byte math, slice sizes per format, hand-built decoder blocks, pak
  round trips (encode, load, decode) for BC1/BC3/BC4/BC5 at quality 0 and 2, 2D and a 3-slice
  array with 6 levels, arrays staying RGBA8 unless `cookTextureArrays` is set, an unknown target falling back to RGBA8 with the baked chain, an
  already-compressed array shipping unchanged, and TXMI blobs with format byte 1/2/3 (old files) and 4/5/6.
- Tolerances (max abs error per channel, smooth ramp): BC1/BC3 within 20 at level 0 of a 16x16
  image; in the array test 28 down to 8x4, 64 for the 4x2 and 2x1 tail (a whole gradient in one
  block, measured 58), 12 for 1x1; BC4/BC5 within 6.
- Full `he_tests` run: 3 failures, all pre-existing: `test_hpak` `scanContentDirectory` (6 vs 2, also before this step), and `test_profiler` :107 / :215, which pass 18/18 when the file runs alone (load-sensitive). `test_hpak` alone: 58/59 (that one failure).
- `test_hpak` (58/59),
  `test_contentmanager`, `test_texture_viewer`, `test_terrain_heightmap`, `test_culling`,
  `test_auto_landscape_variants` pass.
- `scripts/bcn-gl-probe/probe.cpp` on Apple GL 4.1 (M5): BC1, BC3 (both with and without sRGB),
  BC4 and BC5 upload as a 3-slice, 5-level `GL_TEXTURE_2D_ARRAY` through the gather path with
  `glGetError() == 0`, and sampling every slice and level returns the colour that went in (worst error
  0.0065, 0.0 for RGTC). This also settles the open plan §11 item: **`GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT` is accepted on this Mac's GL**.
- `VulkanRenderer.cpp`: `c++ -fsyntax-only` against the MoltenVK headers passes, negative control (a typo in the new code) fails as it must.

## Not verified

- **D3D11, D3D12**: no Windows toolchain here; the diff was read, not compiled. Windows CI is the judge. *Update (step 11): CI run 39548430947 on `fbc999fd` is green for Windows, Linux and Linux · Vulkan (lavapipe), so the D3D11/D3D12/Vulkan changes compile there. That is a compile result only, no BCn upload ran on a GPU.*
- **Vulkan, D3D at run time** (images, the RGBA8 fallback branch on a device without BC), Metal BC1/BC4/BC5 (Apple Silicon takes ASTC; the BC paths need an Intel/AMD Mac), Linux CI.
- The GL fallback branch (decode to RGBA8) was not executed: this GL supports everything it is asked.
- Encode speed of BC1/BC3 vs BC7 on 2K arrays: not measured.

## Decisions taken

- Array cooking is built (plan §7.4 step 3) but opt-in, because of the GI regression above. The Metal array path takes ASTC/BC regardless, so a later opt-in works on every backend.
- BC1 is the opaque RGB variant on GL (`RGB_S3TC_DXT1`) and Vulkan (`BC1_RGB`); D3D/Metal BC1 can carry 1-bit alpha, but the encoder never writes the 3-colour mode, so the textures agree.
- `giAutoLandscapeSliceMeans` decodes BC1/3/4/5 arrays first and still returns false for BC7/ASTC (the GI keeps its default colours then). Decoding a whole 2K array just for 16x16 means is wasteful; whoever ships BCn Auto arrays should pick the small level before decoding, or precompute the means.

## Handoff

- **Topic / step:** Thema 180, step 10 (replaces the stopped step 2)
- **Branch / commit:** `claude/auto-landscape-material-2-0-alle-backends-bcn-instancing-audit`, feature commit `5babcddd`, follow-up commit (opt-in array cooking, this doc) on top; pushed: yes
- **Done:** BC1/BC4/BC5 in the enum, encoders, opt-in array cooking (`PackSettings::cookTextureArrays`, default off), CPU decoders, upload paths on GL/D3D11/D3D12/Vulkan/Metal, tests.
- **Files touched:** `src/HE_Core/include/ContentManager/Assets.h:572-640` (enum + helpers), `TextureArrayBuild.*`, `TextureBlockDecode.*`, `src/HE_Core/src/Hpak/HpakWriter.cpp` (`encodeBc1/4/5`, `cookTexture`), `OpenGLRenderer.cpp` (`glBlockInternalFormat`, `uploadTextureArrayGL`), `D3D11Renderer.cpp` (`createAlbedoSRV`), `D3D12Renderer.cpp` (`uploadTexture2D`), `VulkanRenderer.cpp` (`uploadTextureImage`), `MetalRenderer.mm` (`metalTexPixelFormat`, `uploadMetalTextureArray`), `tests/test_texture_bcn.cpp`.
- **Verified / Not verified:** see the two sections above.
- **Pending:** D3D11/D3D12/Vulkan CI compile and image tests; a hardware pass for the RGBA8 fallback branch.
- **Traps:** editing `Assets.h` rebuilds most of the tree (about 20 min here); `TextureFormat` inside `namespace HE` resolves to `HE::TextureFormat` (Enums.h), write `::TextureFormat`; the export dialog and `PackSettings` still choose ONE format for every texture, so BC4/BC5 are not reachable from the UI (on purpose: a BC5 target would turn every colour texture into RG).
- **Process note:** most edits this step went through python/`cat` heredocs, against the standing 'use the native Edit tools' order.
- **Next steps:** 0. Before anything sets `cookTextureArrays`: carry the GI slice means (see above). 1. Step 5/§7.3: normal Z reconstruction in the Normal nodes before any BC5 normal array is used. 2. Plan §7.5 route B: `landscape_tex_gen --cook bc|astc` sibling assets and the per-capability resolve. 3. Per-texture-kind format choice (albedo BC7, normal BC5, mask BC7) instead of one pack-wide target.
