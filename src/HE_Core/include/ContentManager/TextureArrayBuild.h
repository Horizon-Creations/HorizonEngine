#pragma once
// Texture arrays (Thema 158): assemble same-sized 2D textures into ONE
// TextureAsset with `layers` slices, and the slice/mip arithmetic every backend
// needs to upload one. A Texture Array Sample node reads the result as a
// sampler2DArray — one sampler for any number of slices, which is how five
// terrain layers × Albedo / Normal / Mask fit the four heTexP slots.

#include "Types/Defines.h"
#include "ContentManager/Assets.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HE
{

// Bytes of one mip chain of `mips` levels starting at w × h (each level
// halves, clamped at 1) — the size of ONE slice in TextureAsset::data. `format`
// picks the per-level math: RGBA8 is w*h*4; a block format is
// ceil(w/4)*ceil(h/4)*textureFormatBlockBytes (8 B for BC1/BC4, 16 B otherwise),
// so the 2×2 and 1×1 tail levels still cost one whole block each.
HE_API size_t textureArraySliceBytes(uint32_t w, uint32_t h, uint32_t mips,
                                     ::TextureFormat format = ::TextureFormat::RGBA8);

// Byte offset of (slice, level) in TextureAsset::data for an array of `format`.
HE_API size_t textureArrayOffset(uint32_t w, uint32_t h, uint32_t mips,
                                 uint32_t slice, uint32_t level,
                                 ::TextureFormat format = ::TextureFormat::RGBA8);

// True when `t` is a usable array payload: channels 4, RGBA8 or a block format,
// non-zero size, data holding exactly layers × textureArraySliceBytes in its
// format. The renderers check this before uploading and fall back to white otherwise.
HE_API bool textureArrayPayloadValid(const TextureAsset& t);

// Build `out` (pixels, size, layers, mipLevels, format, srgb) from `slices`:
// every slice must be RGBA8, 4 channels, the same width/height as slice 0, and
// carry at least level 0. With `bakeMips` the full chain is rebuilt from each
// slice's level 0 by a 2×2 box filter (the packer's halving) so all five
// backends sample the SAME levels — GL/Metal would otherwise generate their own
// and D3D/Vulkan would have none. sRGB follows slice 0. id / name / path are
// left to the caller. Returns false (and `err`) when the slices don't match.
HE_API bool buildTextureArray(const std::vector<const TextureAsset*>& slices,
                              TextureAsset& out, bool bakeMips, std::string* err = nullptr);

} // namespace HE
