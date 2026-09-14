#include "HorizonScene/FoliagePaint.h"
#include "HorizonScene/Components/FoliageComponent.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>

namespace FoliagePaint
{

namespace
{
    constexpr uint32_t kMinRes = 8;
    constexpr uint32_t kMaxRes = 4096;

    uint8_t toByte(float v)
    {
        return static_cast<uint8_t>(std::clamp(
            static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)), 0, 255));
    }
}

void ensureMask(FoliageComponent& fol)
{
    const uint32_t mr = std::clamp(fol.maskRes, kMinRes, kMaxRes);
    fol.maskRes = mr;
    const size_t want = static_cast<size_t>(mr) * mr;
    if (fol.densityMask.size() == want) return;

    // Everything at full density — identical to the uniform scatter of a layer
    // without a mask, so allocating the map never changes the layout.
    fol.densityMask.assign(want, 255);
    fol.dirty = true;
}

void clearMask(FoliageComponent& fol)
{
    if (fol.densityMask.empty()) return;
    fol.densityMask.clear();
    fol.dirty = true;
}

void fillMask(FoliageComponent& fol, float value)
{
    ensureMask(fol);
    std::fill(fol.densityMask.begin(), fol.densityMask.end(), toByte(value));
    fol.dirty = true;
}

bool paint(FoliageComponent& fol, const TerrainComponent& tc,
           float localX, float localZ,
           float radius, float falloff, float strength, float target)
{
    if (tc.sizeX <= 0.0f || tc.sizeZ <= 0.0f) return false;

    radius   = std::max(0.0f, radius);
    falloff  = std::max(0.0f, falloff);
    strength = std::clamp(strength, 0.0f, 1.0f);
    target   = std::clamp(target,   0.0f, 1.0f);
    const float outer = radius + falloff;
    // Before the mask is allocated: a stroke that cannot change anything (a
    // dt == 0 frame gives strength 0) must not leave the layer "painted" —
    // that would lock the resolution slider for nothing.
    if (outer <= 0.0f || strength <= 0.0f) return false;

    ensureMask(fol);
    if (fol.densityMask.empty()) return false;

    const uint32_t mr    = fol.maskRes;
    const float    halfX = tc.sizeX * 0.5f;
    const float    halfZ = tc.sizeZ * 0.5f;

    // World rect → texel rect. The mask spans the terrain's 0..1 UV range, so
    // one texel is sizeX/mr wide.
    const float texelX = tc.sizeX / static_cast<float>(mr);
    const float texelZ = tc.sizeZ / static_cast<float>(mr);
    auto toTexel = [](float v, float lo, float size, uint32_t n) {
        return (v - lo) / size * static_cast<float>(n);
    };
    const int x0 = std::max(0, static_cast<int>(std::floor(toTexel(localX - outer, -halfX, tc.sizeX, mr))));
    const int x1 = std::min<int>(mr - 1, static_cast<int>(std::ceil(toTexel(localX + outer, -halfX, tc.sizeX, mr))));
    const int z0 = std::max(0, static_cast<int>(std::floor(toTexel(localZ - outer, -halfZ, tc.sizeZ, mr))));
    const int z1 = std::min<int>(mr - 1, static_cast<int>(std::ceil(toTexel(localZ + outer, -halfZ, tc.sizeZ, mr))));
    if (x0 > x1 || z0 > z1) return false;

    bool any = false;
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

            uint8_t& px = fol.densityMask[static_cast<size_t>(tz) * mr + tx];
            float v = static_cast<float>(px) / 255.0f;
            v = v * (1.0f - a) + target * a;
            // Snap the last step: a lerp toward 0 or 1 has a fixpoint just short
            // of it once the move is below the byte quantum, so an erased spot
            // would forever keep 1/255 — enough for the odd bush to survive in a
            // "cleared" yard. Same reason TerrainPaint snaps its residue.
            if (std::abs(v - target) < 1.0f / 255.0f) v = target;
            px  = toByte(v);
            any = true;
        }
    }

    if (any) fol.dirty = true;
    return any;
}

float sample(const FoliageComponent& fol, const TerrainComponent& tc,
             float localX, float localZ)
{
    const uint32_t mr = fol.maskRes;
    if (fol.densityMask.size() != static_cast<size_t>(mr) * mr || mr == 0) return 1.0f;
    if (tc.sizeX <= 0.0f || tc.sizeZ <= 0.0f) return 1.0f;

    // Texel-centre aligned: local -halfX maps to texel centre -0.5, so a point
    // in the middle of texel i reads exactly texel i (matches paint()).
    const float gx = std::clamp((localX + tc.sizeX * 0.5f) / tc.sizeX * static_cast<float>(mr) - 0.5f,
                                0.0f, static_cast<float>(mr - 1));
    const float gz = std::clamp((localZ + tc.sizeZ * 0.5f) / tc.sizeZ * static_cast<float>(mr) - 0.5f,
                                0.0f, static_cast<float>(mr - 1));
    const uint32_t xi0 = static_cast<uint32_t>(gx);
    const uint32_t zi0 = static_cast<uint32_t>(gz);
    const uint32_t xi1 = std::min(xi0 + 1, mr - 1);
    const uint32_t zi1 = std::min(zi0 + 1, mr - 1);
    const float fx = gx - static_cast<float>(xi0);
    const float fz = gz - static_cast<float>(zi0);
    auto at = [&](uint32_t x, uint32_t z) {
        return static_cast<float>(fol.densityMask[static_cast<size_t>(z) * mr + x]) / 255.0f;
    };
    const float v = at(xi0, zi0) * (1 - fx) * (1 - fz) + at(xi1, zi0) * fx * (1 - fz)
                  + at(xi0, zi1) * (1 - fx) * fz       + at(xi1, zi1) * fx * fz;
    return std::clamp(v, 0.0f, 1.0f);
}

float coverage(const FoliageComponent& fol)
{
    if (fol.densityMask.empty()) return 1.0f;
    double sum = 0.0;
    for (const uint8_t b : fol.densityMask) sum += b;
    return static_cast<float>(sum / (255.0 * static_cast<double>(fol.densityMask.size())));
}

} // namespace FoliagePaint
