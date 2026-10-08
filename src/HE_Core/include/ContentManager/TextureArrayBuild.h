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

// Bytes of one RGBA8 mip chain of `mips` levels starting at w × h (each level
// halves, clamped at 1) — the size of ONE slice in TextureAsset::data.
HE_API size_t textureArraySliceBytes(uint32_t w, uint32_t h, uint32_t mips);

// Byte offset of (slice, level) in TextureAsset::data for an RGBA8 array.
HE_API size_t textureArrayOffset(uint32_t w, uint32_t h, uint32_t mips,
                                 uint32_t slice, uint32_t level);

// True when `t` is a usable RGBA8 array payload: channels 4, RGBA8, non-zero
// size, data holding exactly layers × textureArraySliceBytes. The renderers
// check this before uploading and fall back to white otherwise.
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
