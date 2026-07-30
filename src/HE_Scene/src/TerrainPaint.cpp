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
    if (tc.layerWeights.size() == want) return;

    // Fully on layer 0 — identical to the 1x1 (1,0,0,0) default the renderer binds
    // for an unpainted landscape, so allocating the map never changes the look.
    tc.layerWeights.assign(want, 0);
    for (size_t i = 0; i < want; i += 4) tc.layerWeights[i] = 255;
    tc.weightsDirty = true;
}

bool paint(TerrainComponent& tc, float localX, float localZ,
           int layer, float radius, float falloff, float strength)
{
    if (layer < 0 || layer > 3) return false;
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

            uint8_t* px = &tc.layerWeights[(static_cast<size_t>(tz) * wr + tx) * 4];
            // Move toward "all `layer`" by `a`, keeping the sum at 255: the target
            // channel rises, the others are scaled down by the same factor. Doing
            // it as a lerp (rather than add + renormalise) keeps repeated strokes
            // converging instead of overshooting.
            float w[4];
            for (int k = 0; k < 4; ++k) w[k] = static_cast<float>(px[k]) / 255.0f;
            float sum = w[0] + w[1] + w[2] + w[3];
            if (sum <= 1e-5f) { w[0] = 1.0f; w[1] = w[2] = w[3] = 0.0f; sum = 1.0f; }
            for (int k = 0; k < 4; ++k) w[k] /= sum;
            for (int k = 0; k < 4; ++k)
                w[k] = w[k] * (1.0f - a) + (k == layer ? a : 0.0f);

            // Snap a residue below the byte quantisation floor to zero. Without
            // this the lerp has a fixpoint just short of full: a leftover weight
            // of 0.5/255 rounds back up to 1 every stroke, so a layer painted at
            // full strength forever sat at 254/1 instead of reaching 255/0.
            float keep = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                if (k != layer && w[k] < 1.0f / 255.0f) w[k] = 0.0f;
                keep += w[k];
            }
            if (keep > 1e-5f) for (int k = 0; k < 4; ++k) w[k] /= keep;

            // Quantise so the bytes still sum to exactly 255 — a drifting sum
            // would slowly darken or brighten the blend (the shader normalises,
            // but rounding 4 channels independently loses up to 3/255 per stroke).
            int q[4], total = 0;
            for (int k = 0; k < 4; ++k)
            { q[k] = static_cast<int>(std::lround(w[k] * 255.0f)); total += q[k]; }
            q[layer] += 255 - total;                       // absorb the residue
            for (int k = 0; k < 4; ++k)
                px[k] = static_cast<uint8_t>(std::clamp(q[k], 0, 255));
        }
    }

    tc.weightsDirty = true;
    return true;
}

} // namespace TerrainPaint
