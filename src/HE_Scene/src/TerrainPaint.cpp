#include "HorizonScene/TerrainPaint.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>

namespace TerrainPaint
{

void ensureWeightmap(TerrainComponent& tc)
{
    const uint32_t wr = std::clamp(tc.weightRes, 1u, 4096u);
    tc.weightRes = wr;
    const size_t want = static_cast<size_t>(wr) * wr * 4;
    // A second page only means something beside a first page of the same size.
    if (!tc.groupWeights.empty() && tc.groupWeights.size() != want) tc.groupWeights.clear();
    if (!tc.layerWeights2.empty() && tc.layerWeights2.size() != want)
    {
        tc.layerWeights2.clear();
        tc.weightsDirty = true;
    }
    if (tc.layerWeights.size() == want) return;

    // Fully on layer 0 — identical to the 1x1 (1,0,0,0) default the renderer binds
    // for an unpainted landscape, so allocating the map never changes the look.
    tc.layerWeights.assign(want, 0);
    for (size_t i = 0; i < want; i += 4) tc.layerWeights[i] = 255;
    tc.layerWeights2.clear();   // its texels described the old first page
    tc.groupWeights.clear();
    tc.weightsDirty = true;
}

bool paint(TerrainComponent& tc, float localX, float localZ,
           int layer, float radius, float falloff, float strength)
{
    if (layer < 0 || layer >= kTerrainMaxLayers) return false;
    ensureWeightmap(tc);
    if (tc.layerWeights.empty()) return false;

    const uint32_t wr    = tc.weightRes;
    const float    halfX = tc.sizeX * 0.5f;
    const float    halfZ = tc.sizeZ * 0.5f;
    if (tc.sizeX <= 0.0f || tc.sizeZ <= 0.0f) return false;

    radius   = std::max(0.0f, radius);
    falloff  = std::max(0.0f, falloff);
    strength = std::clamp(strength, 0.0f, 1.0f);
    const float outer = radius + falloff;
    if (outer <= 0.0f || strength <= 0.0f) return false;

    // World rect → texel rect. The weightmap spans the terrain's 0..1 UV range,
    // so one texel is sizeX/wr wide.
    const float texelX = tc.sizeX / static_cast<float>(wr);
    const float texelZ = tc.sizeZ / static_cast<float>(wr);
    auto toTexel = [](float v, float lo, float size, uint32_t n) {
        return (v - lo) / size * static_cast<float>(n);
    };
    const int x0 = std::max(0, static_cast<int>(std::floor(toTexel(localX - outer, -halfX, tc.sizeX, wr))));
    const int x1 = std::min<int>(wr - 1, static_cast<int>(std::ceil(toTexel(localX + outer, -halfX, tc.sizeX, wr))));
    const int z0 = std::max(0, static_cast<int>(std::floor(toTexel(localZ - outer, -halfZ, tc.sizeZ, wr))));
    const int z1 = std::min<int>(wr - 1, static_cast<int>(std::ceil(toTexel(localZ + outer, -halfZ, tc.sizeZ, wr))));
    if (x0 > x1 || z0 > z1) return false;

    // Layers 4..7 live on the second page. The first stroke that needs it
    // allocates it all zero — every texel keeps its mix, now spelled over eight
    // channels. Without it the loop below runs over four channels and does
    // exactly what painting did before there were eight layers.
    const size_t pageBytes = tc.layerWeights.size();
    if (layer >= 4 && tc.layerWeights2.size() != pageBytes)
        tc.layerWeights2.assign(pageBytes, 0);
    const int nAll = tc.layerWeights2.size() == pageBytes ? kTerrainMaxLayers : 4;
    // `ch[i]` is the layer of the i-th entry; `target` is the entry of `layer` itself.
    int ch[kTerrainMaxLayers], n = 0;
    for (int k = 0; k < nAll; ++k) ch[n++] = k;
    const int target = layer;

    for (int tz = z0; tz <= z1; ++tz)
    {
        // Texel CENTRE in terrain-local world units.
        const float wz = -halfZ + (static_cast<float>(tz) + 0.5f) * texelZ;
        for (int tx = x0; tx <= x1; ++tx)
        {
            const float wx = -halfX + (static_cast<float>(tx) + 0.5f) * texelX;
            const float dx = wx - localX, dz = wz - localZ;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > outer) continue;
            // Full strength inside `radius`, linear to 0 across `falloff`.
            const float fall = (dist <= radius || falloff <= 0.0f)
                ? 1.0f : 1.0f - (dist - radius) / falloff;
            const float a = std::clamp(strength * fall, 0.0f, 1.0f);
            if (a <= 0.0f) continue;

            const size_t at = (static_cast<size_t>(tz) * wr + tx) * 4;
            uint8_t* px[kTerrainMaxLayers];
            for (int k = 0; k < n; ++k)
                px[k] = ch[k] < 4 ? &tc.layerWeights[at + ch[k]] : &tc.layerWeights2[at + ch[k] - 4];
            // Move toward "all `layer`" by `a`, keeping the sum at 255: the target
            // channel rises, the others are scaled down by the same factor. Doing
            // it as a lerp (rather than add + renormalise) keeps repeated strokes
            // converging instead of overshooting.
            float w[kTerrainMaxLayers];
            float sum = 0.0f;
            for (int k = 0; k < n; ++k) { w[k] = static_cast<float>(*px[k]) / 255.0f; sum += w[k]; }
            if (sum <= 1e-5f)
            {
                for (int k = 0; k < n; ++k) w[k] = 0.0f;
                w[0] = 1.0f; sum = 1.0f;
            }
            for (int k = 0; k < n; ++k) w[k] /= sum;
            for (int k = 0; k < n; ++k)
                w[k] = w[k] * (1.0f - a) + (k == target ? a : 0.0f);

            // Snap a residue below the byte quantisation floor to zero. Without
            // this the lerp has a fixpoint just short of full: a leftover weight
            // of 0.5/255 rounds back up to 1 every stroke, so a layer painted at
            // full strength forever sat at 254/1 instead of reaching 255/0.
            float keep = 0.0f;
            for (int k = 0; k < n; ++k)
            {
                if (k != target && w[k] < 1.0f / 255.0f) w[k] = 0.0f;
                keep += w[k];
            }
            if (keep > 1e-5f) for (int k = 0; k < n; ++k) w[k] /= keep;

            // Quantise so the bytes still sum to exactly 255 — a drifting sum
            // would slowly darken or brighten the blend (the shader normalises,
            // but rounding the channels independently loses up to n-1 /255 per
            // stroke).
            int q[kTerrainMaxLayers], total = 0;
            for (int k = 0; k < n; ++k)
            { q[k] = static_cast<int>(std::lround(w[k] * 255.0f)); total += q[k]; }
            q[target] += 255 - total;                       // absorb the residue
            for (int k = 0; k < n; ++k)
                *px[k] = static_cast<uint8_t>(std::clamp(q[k], 0, 255));
        }
    }

    tc.weightsDirty = true;
    return true;
}

bool fillLayer(TerrainComponent& tc, int layer)
{
    if (layer < 0 || layer >= kTerrainMaxLayers) return false;
    ensureWeightmap(tc);
    if (tc.layerWeights.empty()) return false;

    const size_t bytes = tc.layerWeights.size();
    std::fill(tc.layerWeights.begin(), tc.layerWeights.end(), uint8_t{ 0 });
    if (layer >= 4)
        tc.layerWeights2.assign(bytes, 0);
    else
        tc.layerWeights2.clear();   // nothing on page 1: it is "all zero" without the bytes
    std::vector<uint8_t>& page = layer < 4 ? tc.layerWeights : tc.layerWeights2;
    for (size_t i = static_cast<size_t>(layer % 4); i < bytes; i += 4) page[i] = 255;
    tc.weightsDirty = true;
    return true;
}

bool texelWeights(const TerrainComponent& tc, uint32_t tx, uint32_t tz, uint8_t* out)
{
    const uint32_t wr = tc.weightRes;
    const size_t page = static_cast<size_t>(wr) * wr * 4;
    if (wr == 0 || tx >= wr || tz >= wr || tc.layerWeights.size() != page) return false;
    const size_t at = (static_cast<size_t>(tz) * wr + tx) * 4;
    const bool second = tc.layerWeights2.size() == page;
    for (int k = 0; k < 4; ++k)
    {
        out[k]     = tc.layerWeights[at + k];
        out[k + 4] = second ? tc.layerWeights2[at + k] : 0;
    }
    return true;
}

// ── Mixed texture groups ─────────────────────────────────────────────────────

int mixSlotMask(const TerrainComponent& tc)
{
    int m = 0;
    for (int k = 0; k < kMixGroupSlots; ++k) if (tc.mixGroup[k] >= 0) m |= 1 << k;
    return m;
}

namespace
{
    // The group shares of one texel by slot (0 = own group, 1..3 = mixGroup[0..2]) out of its
    // three share bytes `b`. A slot with no mix group is 0, shares that add past 1 are
    // scaled to fit, the own group is the rest.
    void sharesOf(const TerrainComponent& tc, const uint8_t* b, float* out)
    {
        float t = 0.0f;
        for (int k = 0; k < kMixGroupSlots; ++k)
        {
            out[k + 1] = tc.mixGroup[k] >= 0 ? static_cast<float>(b[k]) / 255.0f : 0.0f;
            t += out[k + 1];
        }
        if (t > 1.0f) { for (int k = 1; k <= kMixGroupSlots; ++k) out[k] /= t; t = 1.0f; }
        out[0] = std::max(0.0f, 1.0f - t);
    }

    // The group grid allocated (zero) beside the first page.
    void ensureGroupGrid(TerrainComponent& tc)
    {
        if (tc.groupWeights.size() != tc.layerWeights.size())
            tc.groupWeights.assign(tc.layerWeights.size(), 0);
    }
}

bool groupShares(const TerrainComponent& tc, uint32_t tx, uint32_t tz, float* out4)
{
    const uint32_t wr = tc.weightRes;
    const size_t page = static_cast<size_t>(wr) * wr * 4;
    if (wr == 0 || tx >= wr || tz >= wr || tc.layerWeights.size() != page) return false;
    const size_t at = (static_cast<size_t>(tz) * wr + tx) * 4;
    uint8_t b[kMixGroupSlots] = {};
    if (tc.groupWeights.size() == page)
        for (int k = 0; k < kMixGroupSlots; ++k) b[k] = tc.groupWeights[at + k];
    sharesOf(tc, b, out4);
    return true;
}

bool setMixGroup(TerrainComponent& tc, int k, int group)
{
    if (k < 0 || k >= kMixGroupSlots) return false;
    group = std::max(group, -1);
    if (tc.mixGroup[k] == group) return false;
    if (group >= 0)
        for (int o = 0; o < kMixGroupSlots; ++o)
            if (o != k && tc.mixGroup[o] == group) return false;

    // A new slot starts at share 0 and a cleared one is dropped, so the texels fall back to
    // the own group. Re-pointing a live slot at another group keeps its bytes, as switching
    // the terrain's group keeps the paint.
    if (tc.groupWeights.size() == tc.layerWeights.size() && !tc.groupWeights.empty()
        && (tc.mixGroup[k] < 0 || group < 0))
    {
        for (size_t i = static_cast<size_t>(k); i < tc.groupWeights.size(); i += 4) tc.groupWeights[i] = 0;
        tc.weightsDirty = true;
    }
    tc.mixGroup[k] = group;
    return true;
}

bool paintGroup(TerrainComponent& tc, float localX, float localZ,
                int slot, float radius, float falloff, float strength)
{
    if (slot < 0 || slot > kMixGroupSlots) return false;
    if (slot > 0 && tc.mixGroup[slot - 1] < 0) return false;
    ensureWeightmap(tc);
    if (tc.layerWeights.empty()) return false;
    if (tc.sizeX <= 0.0f || tc.sizeZ <= 0.0f) return false;

    const uint32_t wr    = tc.weightRes;
    const float    halfX = tc.sizeX * 0.5f;
    const float    halfZ = tc.sizeZ * 0.5f;
    radius   = std::max(0.0f, radius);
    falloff  = std::max(0.0f, falloff);
    strength = std::clamp(strength, 0.0f, 1.0f);
    const float outer = radius + falloff;
    if (outer <= 0.0f || strength <= 0.0f) return false;

    const float texelX = tc.sizeX / static_cast<float>(wr);
    const float texelZ = tc.sizeZ / static_cast<float>(wr);
    auto toTexel = [](float v, float lo, float size, uint32_t n) {
        return (v - lo) / size * static_cast<float>(n);
    };
    const int x0 = std::max(0, static_cast<int>(std::floor(toTexel(localX - outer, -halfX, tc.sizeX, wr))));
    const int x1 = std::min<int>(wr - 1, static_cast<int>(std::ceil(toTexel(localX + outer, -halfX, tc.sizeX, wr))));
    const int z0 = std::max(0, static_cast<int>(std::floor(toTexel(localZ - outer, -halfZ, tc.sizeZ, wr))));
    const int z1 = std::min<int>(wr - 1, static_cast<int>(std::ceil(toTexel(localZ + outer, -halfZ, tc.sizeZ, wr))));
    if (x0 > x1 || z0 > z1) return false;

    ensureGroupGrid(tc);
    for (int tz = z0; tz <= z1; ++tz)
    {
        const float wz = -halfZ + (static_cast<float>(tz) + 0.5f) * texelZ;
        for (int tx = x0; tx <= x1; ++tx)
        {
            const float wx = -halfX + (static_cast<float>(tx) + 0.5f) * texelX;
            const float dx = wx - localX, dz = wz - localZ;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > outer) continue;
            const float fall = (dist <= radius || falloff <= 0.0f)
                ? 1.0f : 1.0f - (dist - radius) / falloff;
            const float a = std::clamp(strength * fall, 0.0f, 1.0f);
            if (a <= 0.0f) continue;

            uint8_t* b = &tc.groupWeights[(static_cast<size_t>(tz) * wr + tx) * 4];
            // Toward "all `slot`", the same lerp and residue snap as paint(): the shares
            // always sum to 1, the own group is the one that is not stored.
            float w[kMixGroupSlots + 1];
            sharesOf(tc, b, w);
            for (int i = 0; i <= kMixGroupSlots; ++i) w[i] = w[i] * (1.0f - a) + (i == slot ? a : 0.0f);
            float keep = 0.0f;
            for (int i = 0; i <= kMixGroupSlots; ++i)
            {
                if (i != slot && w[i] < 1.0f / 255.0f) w[i] = 0.0f;
                keep += w[i];
            }
            if (keep > 1e-5f) for (int i = 0; i <= kMixGroupSlots; ++i) w[i] /= keep;

            int q[kMixGroupSlots];
            int total = 0;
            for (int i = 0; i < kMixGroupSlots; ++i)
            {
                q[i] = static_cast<int>(std::lround(w[i + 1] * 255.0f));
                total += q[i];
            }
            while (total > 255)   // halves rounding up past the whole: the largest gives it back
            {
                int big = 0;
                for (int i = 1; i < kMixGroupSlots; ++i) if (q[i] > q[big]) big = i;
                --q[big]; --total;
            }
            for (int i = 0; i < kMixGroupSlots; ++i)
                if (tc.mixGroup[i] >= 0) b[i] = static_cast<uint8_t>(q[i]);
        }
    }
    tc.weightsDirty = true;
    return true;
}

bool fillGroup(TerrainComponent& tc, int slot)
{
    if (slot < 0 || slot > kMixGroupSlots) return false;
    if (slot > 0 && tc.mixGroup[slot - 1] < 0) return false;
    ensureWeightmap(tc);
    if (tc.layerWeights.empty()) return false;
    ensureGroupGrid(tc);
    for (size_t i = 0; i < tc.groupWeights.size(); i += 4)
        for (int k = 0; k < kMixGroupSlots; ++k)
            tc.groupWeights[i + k] = (tc.mixGroup[k] >= 0 && slot == k + 1) ? 255 : 0;
    tc.weightsDirty = true;
    return true;
}

bool paintInGroup(TerrainComponent& tc, float localX, float localZ,
                  int layer, int slot, float radius, float falloff, float strength)
{
    const bool tex = paint(tc, localX, localZ, layer, radius, falloff, strength);
    const bool grp = slot >= 0 && mixSlotMask(tc) != 0
        && paintGroup(tc, localX, localZ, slot, radius, falloff, strength);
    return tex || grp;
}

bool usesSecondPage(const TerrainComponent& tc)
{
    const size_t page = static_cast<size_t>(tc.weightRes) * tc.weightRes * 4;
    if (page == 0 || tc.layerWeights.size() != page || tc.layerWeights2.size() != page)
        return false;
    return std::any_of(tc.layerWeights2.begin(), tc.layerWeights2.end(),
                       [](uint8_t b) { return b != 0; });
}

bool usesGroupPage(const TerrainComponent& tc)
{
    const size_t page = static_cast<size_t>(tc.weightRes) * tc.weightRes * 4;
    if (page == 0 || tc.layerWeights.size() != page || tc.groupWeights.size() != page
        || mixSlotMask(tc) == 0)
        return false;
    return std::any_of(tc.groupWeights.begin(), tc.groupWeights.end(),
                       [](uint8_t b) { return b != 0; });
}

std::vector<uint8_t> buildWeightTexture(const TerrainComponent& tc,
                                        uint32_t& width, uint32_t& height, bool withGroupPage)
{
    const uint32_t wr = tc.weightRes;
    width = height = 0;
    if (wr == 0 || tc.layerWeights.size() != static_cast<size_t>(wr) * wr * 4) return {};
    height = wr;
    const bool group = withGroupPage && usesGroupPage(tc);
    if (!group && !usesSecondPage(tc))
    {
        width = wr;
        return tc.layerWeights;
    }
    // Side by side, row by row: [page 0 row z | page 1 row z (| group row z)]. Same rows,
    // same V, so the shader only rescales U. The group page only travels for a material
    // that reads it (a 3:1 map would confuse the 2:1 test of every other landscape blend).
    const uint32_t pages = group ? 3 : 2;
    width = pages * wr;
    const size_t row = static_cast<size_t>(wr) * 4;
    std::vector<uint8_t> out(row * pages * wr, 0);
    const bool second = tc.layerWeights2.size() == tc.layerWeights.size();
    for (uint32_t z = 0; z < wr; ++z)
    {
        std::copy_n(tc.layerWeights.begin() + z * row, row, out.begin() + (pages * z) * row);
        if (second)
            std::copy_n(tc.layerWeights2.begin() + z * row, row, out.begin() + (pages * z + 1) * row);
        if (group)
            std::copy_n(tc.groupWeights.begin() + z * row, row, out.begin() + (pages * z + 2) * row);
    }
    return out;
}

} // namespace TerrainPaint
