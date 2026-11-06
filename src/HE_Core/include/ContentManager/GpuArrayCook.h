#pragma once
// ─── GPU-ready texture arrays in the editor ────────────────────────────────────
// Shared EngineContent holds texture ARRAYS (the landscape's, 13 slices of 2048^2) as RGBA8, the
// platform-neutral raw form: a Mac and a Windows editor read the same file. 291 MB of RGBA8 per
// array then sits in RAM and VRAM, three arrays per landscape group. An export block-compresses
// them for its target (HpakWriter cook), the editor did not.
//
// This does it for the editor: when a loose RGBA8 array is read, it is cooked to the format the
// running GPU samples (BC7 desktop, ASTC 4x4 Metal, BC3 macOS GL) and the cooked file is kept in a
// cache directory keyed by the source file (path, size, mtime), so the encoder runs once per
// download, later starts read the 4x smaller cooked file instead. The cook is the export's own
// (Hpak::cookTextureBlob): every slice and level as it stands, the slice means the GI landscape
// needs baked into CHUNK_TXSM. The file on disk is never touched.
//
// A process-wide setting, not a ContentManager member: the async load workers do not hold a
// ContentManager. Off (format 0) unless an editor turns it on, so the game and every tool read
// arrays exactly as before.
#include <Types/Defines.h>

#include <cstdint>
#include <string>
#include <vector>

namespace HE
{

struct GpuArrayCookSettings
{
    // An HE::TextureFormat value: 0 off (RGBA8 stays), 1 ASTC_4x4, 2 BC7, 3 BC3.
    uint8_t     format  = 0;
    // The encoder effort, 0 fast, 1 balanced, 2 high (Hpak::PackSettings::textureQuality).
    uint8_t     quality = 0;
    // Where cooked arrays are kept. Empty = nothing is cooked (a cook per start is not worth it).
    std::string cacheDir;
    // Files smaller than this are not looked at: arrays that matter are hundreds of MB, and a
    // placeholder array is not worth the encoder.
    uint64_t    minSourceBytes = 8ull << 20;
};

HE_API void                 setGpuArrayCook(const GpuArrayCookSettings& settings);
HE_API GpuArrayCookSettings gpuArrayCook();
HE_API bool                 gpuArrayCookEnabled();

// The format an editor running `backend` should ask for: BC7 on D3D11/D3D12/Vulkan/OpenGL (not on
// Apple, where Vulkan is MoltenVK and OpenGL is 4.1 without BPTC: BC3 for GL), ASTC 4x4 on Metal,
// 0 for the software rasteriser. `HE_EDITOR_ARRAY_COMPRESS` (none|bc7|astc|bc3) overrides it.
// Takes the backend as an int (HE::RendererBackend's value) so this header needs no renderer types.
HE_API uint8_t gpuArrayCookFormatFor(int rendererBackend);

// The .hasset at `sourcePath`, as the GPU wants it: when it is a big loose RGBA8 texture ARRAY and
// the cook is on, `out` receives the cooked file (from the cache, or encoded now and cached) and
// this returns true. False = not applicable (off, small, not an RGBA8 array, unreadable): the
// caller reads the file as usual. A failed encode (no encoder in this build) returns false.
// Thread safe; the cook itself uses all cores.
HE_API bool readGpuCookedArray(const std::string& sourcePath, std::vector<uint8_t>& out);

} // namespace HE
