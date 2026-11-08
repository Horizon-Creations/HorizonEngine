#pragma once
// One RGBA8 image to GPU block-compressed bytes, for runtime callers that cook a SINGLE small picture (the Runtime Virtual
// Texture's cold pages, Thema 333 step 6j/9) rather than a whole pack (Hpak::cookTextureBlob). Thread safe: every thread keeps
// its own encoder context/tables.
#include <Types/Defines.h>

#include <cstdint>
#include <vector>

namespace HE
{

enum class BlockFormat : uint8_t
{
    Astc4x4 = 0,   // 16 bytes per 4x4 block, Apple-Silicon Metal samples it
    Bc7     = 1,   // 16 bytes per 4x4 block, desktop D3D/Vulkan/GL (the Windows step of the same topic)
};

// Bytes of an image `w` x `h` in `format`: ceil(w/4) * ceil(h/4) * 16.
HE_API size_t blockEncodedBytes(uint32_t w, uint32_t h);

// `rgba`: w * h * 4 bytes, row 0 first. `srgb` picks the encoder's colour profile (the bytes are sRGB-encoded colour that a
// *_sRGB GPU format will decode), otherwise linear data (normals, roughness). `quality`: 0 fast, 1 medium, 2 thorough.
// False when this build has no encoder for the format.
HE_API bool encodeBlocks(BlockFormat format, const uint8_t* rgba, uint32_t w, uint32_t h, bool srgb, uint8_t quality,
                         std::vector<uint8_t>& out);

// Frees the calling thread's encoder contexts/tables. A thread that encodes now and then calls this when it goes idle.
HE_API void releaseBlockEncoderCache();

} // namespace HE
