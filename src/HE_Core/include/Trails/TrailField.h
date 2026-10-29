// Trails in snow and sand: where something pressed the ground, how deep, and how fast it heals.
//
// The CPU is the authority (docs/procedural-snow-sand-trails-plan-2026-10-29.md section 3): a sparse
// world-aligned grid of pages, each 128 m x 128 m at 0.25 m per texel, allocated by the first stamp
// and freed when the last texel has healed. Gameplay asks it (sampleTrail: footstep audio, tracking)
// without a GPU readback, the renderer will upload what it needs near the camera (a later step; the
// material functions already take the result: MF_ProceduralSnow / Sand "Disturbance" and
// "Disturbance Slope"). The field knows nothing of the engine's components: the weather comes in as
// a plain struct.
//
// Depth is 0 (untouched) .. 1 (pressed as deep as the surface goes). Pages hold 16 bits per texel, so
// the slowest heal of the plan's defaults (1/1800 per second, 4 ticks a second) still moves a texel
// by several units and never truncates to nothing.
#pragma once

#include <Math/Math.h>
#include <Types/Defines.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace HE
{
// What a stamp pressed: it decides which healing the page follows (a page mixing both uses the
// faster of the two, a known simplification, see the plan's section 3.4).
enum class TrailSurface : uint8_t { Snow = 1, Sand = 2 };

struct TrailSettings
{
    float hSnow  = 1.0f / 120.0f;    // per second at full snowfall: a print fills in two minutes
    float hDrift = 1.0f / 1800.0f;   // per second in a gale: wind drifts snow shut
    float hMelt  = 1.0f / 600.0f;    // per second at full thaw
    float hWind  = 1.0f / 900.0f;    // per second in a gale: wind refills sand (quadratic in the wind)
    float tick   = 0.25f;            // game seconds per heal step (4 Hz)
};

// The weather values that heal a trail, all 0..1 except the wind speed (as on WeatherComponent;
// 1 = calm, 5 = gale). `snowing` is precipType == Snow, `raining` precipType == Rain.
struct TrailWeather
{
    float precip    = 0.0f;
    bool  snowing   = false;
    bool  raining   = false;
    float windSpeed = 1.0f;
    float wetness   = 0.0f;
};

class HE_API TrailField
{
public:
    static constexpr int   kPageCells = 512;
    static constexpr float kCellSize  = 0.25f;                       // metres per texel
    static constexpr float kPageSize  = kPageCells * kCellSize;      // 128 m

    // An ellipse of radii (rx along the yaw direction, rz across it) pressed `depth` deep, soft at
    // the rim; yaw in radians around +Y (0 = +X, as the engine's ground-plane convention). The
    // press is a maximum: stepping into a print does not make it deeper than the deepest foot.
    void stampEllipse(float x, float z, float rx, float rz, float yaw, float depth, TrailSurface surface);
    // A tyre or a sled: a capsule of `radius` along the segment, so a fast mover leaves a line.
    void stampCapsule(float x0, float z0, float x1, float z1, float radius, float depth, TrailSurface surface);

    // Advances the healing to world time `time` (seconds) in fixed steps of settings.tick; the first
    // call only starts the clock. A time that goes backwards moves nothing.
    void tick(double time, const TrailWeather& weather, const TrailSettings& settings = {});

    // Depth at a world position, bilinear between texel centres; 0 where no page exists.
    float sample(float x, float z) const;
    // d(depth)/dx, d(depth)/dz per metre (central difference over one texel): what the material's
    // "Disturbance Slope" input takes.
    glm::vec2 gradient(float x, float z) const;

    size_t pageCount() const { return m_pages.size(); }
    // The texels above zero over all pages (a cheap "is anything trodden" for tests and tools).
    size_t activeTexels() const;
    void   clear() { m_pages.clear(); m_clockStarted = false; }

private:
    struct Page
    {
        std::vector<uint16_t> depth = std::vector<uint16_t>(static_cast<size_t>(kPageCells) * kPageCells, 0);
        uint8_t               surfaces = 0;   // bit set of TrailSurface
        float                 carry = 0.0f;   // heal the last steps could not apply yet, in texel units
    };
    static int64_t key(int px, int pz) { return (static_cast<int64_t>(px) << 32) ^ static_cast<uint32_t>(pz); }

    uint16_t* texelForWrite(int ix, int iz, TrailSurface surface);   // allocates the page
    uint16_t  texel(int ix, int iz) const;                           // 0 off the pages
    float     healRate(uint8_t surfaces, const TrailWeather& w, const TrailSettings& s) const;
    void      healPage(Page& page, float rate, float dt);

    std::unordered_map<int64_t, Page> m_pages;
    bool   m_clockStarted = false;
    double m_clock        = 0.0;
};
} // namespace HE
