#pragma once
// CPU decoders for the BC1 / BC3 / BC4 / BC5 block formats (the S3TC / RGTC family).
// Two jobs: the RGBA8 fallback a backend takes when its GPU or driver can't sample
// one of these (instead of a flat texture), and the round-trip tests of the
// pack-time encoders. BC7 and ASTC are deliberately not here: nothing falls back
// from them to a decode, and the encoders are upstream-tested.
//
// Output matches what the hardware would hand a shader: BC1/BC3 give RGBA (BC1's
// transparent texel, 3-colour mode index 3, decodes to alpha 0), BC4 gives
// (r, 0, 0, 255), BC5 gives (r, g, 0, 255).

#include "Types/Defines.h"
#include "ContentManager/Assets.h"

#include <cstdint>

namespace HE
{

// True for the formats decodeTextureToRGBA8 can expand: BC1, BC3, BC4, BC5.
HE_API bool textureFormatCpuDecodable(::TextureFormat f);

// Decode ONE mip level (w x h texels, tightly packed blocks as the encoders write
// them) into `dstRgba8` (w*h*4 bytes). False when `f` isn't decodable or `srcBytes`
// is shorter than textureLevelBytes(f, w, h).
HE_API bool decodeBlockLevelToRGBA8(::TextureFormat f, const uint8_t* src, size_t srcBytes,
                                    uint32_t w, uint32_t h, uint8_t* dstRgba8);

// Decode every slice and level of `in` (2D or array, any decodable format) into an
// RGBA8 asset with the same size, mips, layers and sRGB flag. id / name / path are
// copied. False (and `out` untouched) when `in` can't be decoded or its payload is
// truncated.
HE_API bool decodeTextureToRGBA8(const TextureAsset& in, TextureAsset& out);

// Mean LINEAR colour of slice `slice` of an array (or of a 2D texture, slice 0), taken
// from the smallest stored level with both sides <= 16 (or the last one) on a <= 16x16
// grid — a few hundred texels, cheap enough to ask every frame — and sRGB-decoded when
// `srgb`. Reads RGBA8 or a decodable block format (BC1/BC3/BC4/BC5); false for BC7 and
// ASTC (no CPU decoder: bake the means at pack time instead, see bakeTextureArraySliceMeans)
// or when the slice lies outside `data`. `data` holds `slice + 1` or more slices of `f`.
HE_API bool textureArraySliceMean(::TextureFormat f, const uint8_t* data, size_t dataBytes,
                                  uint32_t w, uint32_t h, uint32_t mips, bool srgb,
                                  uint32_t slice, float rgb[3]);
HE_API bool textureArraySliceMean(const TextureAsset& t, uint32_t slice, float rgb[3]);

// The means of ALL `layers` slices of a tightly packed RGBA8 array, three floats per slice —
// the payload of CHUNK_TXSM. Same sampling as textureArraySliceMean, so a baked value and a
// value read back from the same RGBA8 pixels agree exactly. False on a truncated payload.
HE_API bool bakeTextureArraySliceMeans(const uint8_t* rgba8, size_t bytes, uint32_t w, uint32_t h,
                                       uint32_t mips, uint32_t layers, bool srgb,
                                       std::vector<float>& out);

} // namespace HE
