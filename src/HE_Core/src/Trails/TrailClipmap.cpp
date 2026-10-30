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

namespace
{
TextureAsset makeTexture(std::vector<uint8_t> pixels)
{
    TextureAsset tex;
    tex.type      = HE::AssetType::Texture;
    tex.id        = HE::kTrailTextureId;
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

namespace
{
std::vector<uint8_t> untouchedPixels()
{
    std::vector<uint8_t> px(static_cast<size_t>(TrailClipmap::kSize) * TrailClipmap::kSize * 4, 128);   // R,G,B,A all 128
    return px;
}
} // namespace

HE::UUID TrailClipmap::ensureTexture(ContentManager& cm)
{
    if (const HE::UUID id = cm.idForPath(kTrailTexturePath); id != HE::UUID{} && cm.getTexture(id)) return id;
    return cm.registerTexture(makeTexture(untouchedPixels()));
}

bool TrailClipmap::fillRect(const TrailField& field, std::vector<uint8_t>& px, int ix0, int iz0, int w, int h)
{
    constexpr int N = kSize;
    const int tw = w + 2, th = h + 2;
    std::vector<int16_t> v(static_cast<size_t>(tw) * th);
    field.copyWindow(ix0 - 1, iz0 - 1, tw, th, v.data());
    auto at = [&](int x, int z) { return static_cast<float>(v[static_cast<size_t>(z + 1) * tw + (x + 1)]) / 32767.0f; };
    const float inv2h = 1.0f / (2.0f * TrailField::kCellSize);
    bool any = false;
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x)
        {
            const int16_t value = v[static_cast<size_t>(z + 1) * tw + (x + 1)];
            uint8_t* p = &px[(static_cast<size_t>(floorMod(iz0 + z, N)) * N + floorMod(ix0 + x, N)) * 4];
            p[0] = static_cast<uint8_t>(std::clamp(128 - static_cast<int>(std::lround(static_cast<float>(value) * (127.0f / 32767.0f))), 1, 255));
            // A flat texel (the vast majority) skips the neighbour reads.
            if (value == 0 && at(x - 1, z) == 0.0f && at(x + 1, z) == 0.0f && at(x, z - 1) == 0.0f && at(x, z + 1) == 0.0f)
                p[1] = p[2] = 128;
            else
            {
                any = true;
                p[1] = slopeByte((at(x + 1, z) - at(x - 1, z)) * inv2h);
                p[2] = slopeByte((at(x, z + 1) - at(x, z - 1)) * inv2h);
            }
            p[3] = 128;
        }
    return any;
}

bool TrailClipmap::update(ContentManager& cm, IRenderer* renderer, const TrailField& field, const glm::vec3& cameraPos)
{
    constexpr int N = kSize;
    const int camX = static_cast<int>(std::floor(cameraPos.x / TrailField::kCellSize));
    const int camZ = static_cast<int>(std::floor(cameraPos.z / TrailField::kCellSize));

    const bool fresh = m_id == HE::UUID{} || cm.getTexture(m_id) == nullptr;
    if (fresh)
    {
        m_id = ensureTexture(cm);
        m_version = ~0ull; m_centred = false; m_hasContent = false;
    }
    // The texture's own pixels, edited in place (no copy, no new asset): the renderer is told below.
    TextureAsset* tex = cm.getTextureMutable(m_id);
    if (!tex) return false;
    if (tex->data.size() != static_cast<size_t>(N) * N * 4)
    {
        tex->data = untouchedPixels();   // someone replaced it with something else: start from a clean window
        m_version = ~0ull; m_centred = false; m_hasContent = false;
    }
    bool wrote = false;
    const bool recentre = !m_centred || std::abs(camX - m_cx) > kRecenterTexels || std::abs(camZ - m_cz) > kRecenterTexels;
    if (recentre)
    {
        m_cx = camX; m_cz = camZ; m_centred = true;
        // The whole window, from the field. Nothing near and nothing in the texture before: it already is the
        // untouched window, not worth a megabyte.
        const bool any = fillRect(field, tex->data, m_cx - N / 2, m_cz - N / 2, N, N);
        if (any || m_hasContent) wrote = true;
        m_hasContent = any;
        m_version = field.version();
    }
    else if (field.version() != m_version)
    {
        std::vector<std::pair<int, int>> tiles;
        field.changedTiles(m_version, tiles);
        const int wx0 = m_cx - N / 2, wz0 = m_cz - N / 2;
        for (const auto& [tx, tz] : tiles)
        {
            // The tile and one texel around it (its edge texels' slopes read across), clipped to the window.
            const int x0 = std::max(tx * TrailField::kTileCells - 1, wx0), x1 = std::min((tx + 1) * TrailField::kTileCells + 1, wx0 + N);
            const int z0 = std::max(tz * TrailField::kTileCells - 1, wz0), z1 = std::min((tz + 1) * TrailField::kTileCells + 1, wz0 + N);
            if (x1 <= x0 || z1 <= z0) continue;
            if (fillRect(field, tex->data, x0, z0, x1 - x0, z1 - z0)) m_hasContent = true;
            wrote = true;
        }
        m_version = field.version();
    }
    if (wrote && renderer) renderer->InvalidateTexture(m_id);
    return wrote || fresh;
}
} // namespace HE
