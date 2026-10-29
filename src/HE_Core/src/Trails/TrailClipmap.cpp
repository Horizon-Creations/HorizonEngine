#include <Trails/TrailClipmap.h>

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Renderer/IRenderer.h>

#include <algorithm>
#include <cmath>

namespace HE
{
namespace
{
inline int floorMod(int a, int b) { const int m = a % b; return m < 0 ? m + b : m; }
inline uint8_t slopeByte(float s)
{
    const float c = std::clamp(s / TrailClipmap::kSlopeRange, -1.0f, 1.0f);
    return static_cast<uint8_t>(128 + static_cast<int>(std::lround(c * 127.0f)));
}
} // namespace

void TrailClipmap::encode(const std::vector<uint16_t>& depth, std::vector<uint8_t>& rgba)
{
    constexpr int N = kSize;
    rgba.assign(static_cast<size_t>(N) * N * 4, 0);
    // The slope is a central difference over the neighbours in the TORUS (the window wraps): at the
    // window's own seam it looks across to the far side, which the material has faded out by then.
    auto d = [&](int x, int z) { return static_cast<float>(depth[static_cast<size_t>(floorMod(z, N)) * N + floorMod(x, N)]) / 65535.0f; };
    const float inv2h = 1.0f / (2.0f * TrailField::kCellSize);
    for (int z = 0; z < N; ++z)
        for (int x = 0; x < N; ++x)
        {
            const uint16_t v = depth[static_cast<size_t>(z) * N + x];
            uint8_t* p = &rgba[(static_cast<size_t>(z) * N + x) * 4];
            const uint8_t depth8 = static_cast<uint8_t>(std::lround(static_cast<float>(v) * (255.0f / 65535.0f)));
            p[0] = static_cast<uint8_t>(255 - depth8);
            // A flat texel (the vast majority) skips the four neighbour reads.
            if (v == 0 && d(x - 1, z) == 0.0f && d(x + 1, z) == 0.0f && d(x, z - 1) == 0.0f && d(x, z + 1) == 0.0f)
                p[1] = p[2] = 128;
            else
            {
                p[1] = slopeByte((d(x + 1, z) - d(x - 1, z)) * inv2h);
                p[2] = slopeByte((d(x, z + 1) - d(x, z - 1)) * inv2h);
            }
            p[3] = 128;
        }
}

namespace
{
TextureAsset makeTexture(std::vector<uint8_t> pixels)
{
    TextureAsset tex;
    tex.type      = HE::AssetType::Texture;
    tex.name      = "trail_clipmap";
    tex.path      = HE::kTrailTexturePath;
    tex.width     = HE::TrailClipmap::kSize;
    tex.height    = HE::TrailClipmap::kSize;
    tex.channels  = 4;
    tex.mipLevels = 1;
    tex.format    = ::TextureFormat::RGBA8;
    tex.srgb      = false;    // data, not colour
    tex.data      = std::move(pixels);
    return tex;
}
} // namespace

HE::UUID TrailClipmap::ensureTexture(ContentManager& cm)
{
    if (const HE::UUID id = cm.idForPath(kTrailTexturePath); id != HE::UUID{} && cm.getTexture(id)) return id;
    std::vector<uint8_t> pixels;
    encode(std::vector<uint16_t>(static_cast<size_t>(kSize) * kSize, 0), pixels);
    return cm.registerTexture(makeTexture(std::move(pixels)));
}

bool TrailClipmap::update(ContentManager& cm, IRenderer* renderer, const TrailField& field, const glm::vec3& cameraPos)
{
    constexpr int N = kSize;
    const int cx = static_cast<int>(std::floor(cameraPos.x / TrailField::kCellSize));
    const int cz = static_cast<int>(std::floor(cameraPos.z / TrailField::kCellSize));
    const int ox = cx - N / 2, oz = cz - N / 2;

    const bool fresh      = m_id == HE::UUID{} || cm.getTexture(m_id) == nullptr;
    if (fresh) { m_id = ensureTexture(cm); m_version = ~0ull; m_hasContent = false; }   // a first call, or a new ContentManager state
    const bool fieldMoved = field.version() != m_version;
    const bool camMoved   = ox != m_originX || oz != m_originZ;
    // An untouched window stays untouched when the camera walks: only a write that CHANGES the picture is worth 1 MB.
    if (!fieldMoved && !(camMoved && m_hasContent)) return fresh;

    std::vector<uint16_t> depth(static_cast<size_t>(N) * N);
    // The window in world texels, then rotated into the torus: pixel (x mod N, z mod N) holds the texel (x, z).
    std::vector<uint16_t> window(static_cast<size_t>(N) * N);
    field.copyWindow(ox, oz, N, N, window.data());
    bool any = false;
    for (int z = 0; z < N; ++z)
        for (int x = 0; x < N; ++x)
        {
            const uint16_t v = window[static_cast<size_t>(z) * N + x];
            if (v) any = true;
            depth[static_cast<size_t>(floorMod(oz + z, N)) * N + floorMod(ox + x, N)] = v;
        }
    m_version = field.version();
    m_originX = ox; m_originZ = oz;
    // Nothing near and nothing written before: the registered texture already is the untouched window.
    if (!any && !m_hasContent) return fresh;
    m_hasContent = any;

    encode(depth, m_pixels);
    cm.replaceTexture(m_id, makeTexture(m_pixels));
    if (renderer) renderer->InvalidateTexture(m_id);
    return true;
}
} // namespace HE
