// The renderer's view of a TrailField: a 128 m x 128 m window around the camera as an ordinary RGBA8
// texture asset (kTrailTexturePath), which the landscape material reads through its fourth graph
// texture slot — no renderer code knows about trails, the usual ContentManager::replaceTexture +
// IRenderer::InvalidateTexture path of the landscape weightmap carries it.
//
// The window is WORLD-ALIGNED and toroidal: the texel of the world position (x, z) is
// (floor(x / 0.25) mod 512, floor(z / 0.25) mod 512), so the shader needs no window origin, only
// `uv = xz / 128` with the sampler's repeat wrap. It is correct within 64 m of the camera; farther
// out the texels belong to places near the camera, so the material fades the trail out before that
// (AutoLandscapeMaterial.cpp: kTrailFadeStart / kTrailFadeEnd).
//
// Texel layout (what AutoLandscapeMaterial decodes), v = the field's signed value, pit depth positive:
//   R = 128 - round(v * 127)   128 = untouched, below = a pit (R 1 = fully deep), above = a heap
//   G = 128 + clamp(dv/dx / 8) * 127     the slope per metre, 128 = flat
//   B = 128 + clamp(dv/dz / 8) * 127
//   A = 128            a marker: the material only trusts texels whose alpha is about one half
//                      (a missing texture's white or black stand-in is 1 or 0 there)
#pragma once

#include <Math/Math.h>
#include <Trails/TrailField.h>
#include <Types/Defines.h>
#include <Types/UUID.h>

#include <cstdint>
#include <vector>

class ContentManager;
class IRenderer;

namespace HE
{
// A virtual path on purpose: no file, so the EngineContent resolver (which scans a material's bytes for
// "Engine/....hasset") never tries to fetch it.
inline constexpr char kTrailTexturePath[] = "mem://HorizonTrails";

class HE_API TrailClipmap
{
public:
    static constexpr int   kSize      = 512;
    static constexpr float kWorldSize = TrailField::kPageSize;   // 128 m: the texture's repeat length
    static constexpr float kSlopeRange = 8.0f;                     // metre-1 mapped to +-127 counts
    // The window is re-centred on the camera only after it has moved this many texels (6 m) from the centre of
    // the last one: the material fades trails out at 58 m, the window is valid to 64 m, and in between only the
    // tiles the field changed are rewritten.
    static constexpr int   kRecenterTexels = 24;

    // Brings the texture in line with the field around `cameraPos`: registers it on the first call (an
    // all-untouched window, so the material can resolve the path from the first frame), then keeps it current:
    // a full rewrite when the camera has left the window's middle, otherwise only the tiles the field changed
    // (a dragged track costs a few tiles, not the 1 MB window). Returns true when the texture was written.
    // `renderer` may be null (tests, tools).
    bool update(ContentManager& cm, IRenderer* renderer, const TrailField& field, const glm::vec3& cameraPos);

    // The texture asset, registered untouched if it is not there yet. ContentManager::loadAsset calls this for
    // kTrailTexturePath, so a material that asks for the path before the first scene tick (the landscape's
    // first draw) finds it instead of logging a miss the renderer would cache.
    static HE::UUID ensureTexture(ContentManager& cm);

    HE::UUID textureId() const { return m_id; }
    // The 512 x 512 x 4 bytes of the last write (tests).
    const std::vector<uint8_t>& pixels() const { return m_pixels; }

private:
    // Recomputes the pixels of the world texels [ix0, ix0 + w) x [iz0, iz0 + h) from the field (a one-texel margin
    // is read for the slopes). Returns whether any of them is not flat.
    bool fillRect(const TrailField& field, int ix0, int iz0, int w, int h);

    HE::UUID             m_id{};
    uint64_t             m_version  = ~0ull;
    int                  m_cx = 0, m_cz = 0;       // the window's centre, texels
    bool                 m_centred = false;        // m_cx/m_cz were set (and the pixels belong to them)
    bool                 m_hasContent = false;     // the texture holds a trodden texel
    std::vector<uint8_t> m_pixels;
};
} // namespace HE
