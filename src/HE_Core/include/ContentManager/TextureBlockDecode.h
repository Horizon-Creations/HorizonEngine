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

} // namespace HE
