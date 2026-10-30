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
// A texel holds one SIGNED number v in [-1, 1]: v > 0 is a pit of that depth (1 = pressed as deep as the
// surface goes), v < 0 a heap of that height (snow or sand pushed aside), 0 untouched. Pages hold it in 16
// bits, so the slowest heal of the plan's defaults (1/1800 per second, 4 ticks a second) still moves a texel
// by several units and never truncates to nothing.
//
// Two ways to leave a mark, and they differ on purpose:
//   press   (stampEllipse, stampCapsule)  only a pit: a footprint, a wheel set down. Nothing is piled up.
//   plow    (plowCapsule)                 a groove dragged along a segment WITH the displaced material heaped
//                                         along both sides: a sled, a plough, a foot dragged through, a tyre
//                                         that digs in. Call it every frame with the last and the new position
//                                         and the track is continuous. A pit always wins over a heap on the
//                                         same texel (the next stretch of the groove clears the heap at its front).
#pragma once

#include <Math/Math.h>
#include <Types/Defines.h>

#include <array>
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
    // The terrain's real geometry: a pit of value 1 lowers the ground this many metres, a heap raises it by
    // v x this (only where the landscape refines its chunks: TerrainComponent::tessellationFactor).
    float displacementMeters = 0.30f;
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

class TrailField;

// A copy of a rectangle of the field for many bilinear reads: what a mesh builder wants, since one chunk asks
// for tens of thousands of positions (each a hash lookup on the field itself).
class HE_API TrailWindow
{
public:
    // The signed value v (pit positive) at a world position; 0 outside the rectangle.
    float sample(float x, float z) const;
    bool  any() const { return m_any; }

private:
    friend class TrailField;
    int  m_ix0 = 0, m_iz0 = 0, m_w = 0, m_h = 0;
    bool m_any = false;
    std::vector<int16_t> m_d;
};

class HE_API TrailField
{
public:
    static constexpr int   kPageCells = 512;
    static constexpr float kCellSize  = 0.25f;                       // metres per texel
    static constexpr float kPageSize  = kPageCells * kCellSize;      // 128 m
    // Changes are tracked per TILE of a page (64 texels = 16 m, 8 x 8 a page), so a track dragged through one
    // corner of a page tells the 16 m around it, not the 128 m.
    static constexpr int   kTileCells = 64;
    static constexpr int   kTilesPerSide = kPageCells / kTileCells;

    // An ellipse of radii (rx along the yaw direction, rz across it) pressed `depth` deep, soft at
    // the rim; yaw in radians around +Y (0 = +X, as the engine's ground-plane convention). The
    // press is a maximum: stepping into a print does not make it deeper than the deepest foot.
    void stampEllipse(float x, float z, float rx, float rz, float yaw, float depth, TrailSurface surface);
    // A tyre or a sled set down: a capsule of `radius` along the segment, so a fast mover leaves a line of
    // pits. No heap beside it (that is plowCapsule).
    void stampCapsule(float x0, float z0, float x1, float z1, float radius, float depth, TrailSurface surface);
    // Dragged through: the capsule's pit AND a heap along both sides, `bermWidth` metres wide outside the
    // groove and `bermHeight` (0..1, like depth) tall at its crest, smooth on both flanks. A heap only grows
    // on ground that is not already a pit, and the higher of two heaps stays.
    void plowCapsule(float x0, float z0, float x1, float z1, float radius, float depth,
                     float bermWidth, float bermHeight, TrailSurface surface);

    // Advances the healing to world time `time` (seconds) in fixed steps of settings.tick; the first
    // call only starts the clock. A time that goes backwards moves nothing.
    void tick(double time, const TrailWeather& weather, const TrailSettings& settings = {});

    // The signed value v (pit depth positive, heap height negative) at a world position, bilinear between
    // texel centres; 0 where no page exists. A footstep wants max(v, 0).
    float sample(float x, float z) const;
    // dv/dx, dv/dz per metre (central difference over one texel): what the material's "Disturbance Slope"
    // input takes.
    glm::vec2 gradient(float x, float z) const;

    // Texels [ix0, ix0 + w) x [iz0, iz0 + h) (global texel indices: x / kCellSize, floored) as signed 16-bit
    // values (+-32767 = +-1), row-major, zero where no page exists. What the renderer's clipmap copies per frame.
    void copyWindow(int ix0, int iz0, int w, int h, int16_t* out) const;
    // Counts up whenever a stamp or a heal step changed anything: a cache keyed on it is current.
    uint64_t version() const { return m_version; }
    // The newest change inside a world rectangle (0 = nothing was ever written there): a mesh built from this
    // rectangle is current while the number is the one it was built with. Pages that healed away and were
    // freed still count, so a mesh that has their pits in it is told to let them go.
    uint64_t regionVersion(float minX, float minZ, float maxX, float maxZ) const;
    // Every tile (global tile indices: texel / kTileCells, floored) that changed after version `since`, healed away
    // pages included. What the clipmap texture rewrites instead of the whole window.
    void changedTiles(uint64_t since, std::vector<std::pair<int, int>>& out) const;
    // The rectangle, copied (with a one-texel margin so slopes at its edge are right).
    TrailWindow window(float minX, float minZ, float maxX, float maxZ) const;

    // ── Persistence (a savegame, a streamed tile) ───────────────────────────────────────────────
    // The field as bytes: every page's key, surface kinds and its 16-bit values, run-length coded (a trodden page is
    // mostly zeros: a few hundred bytes for a footpath, 512 KB dense only for a page walked flat), plus the clock the
    // healing ran on. deserialize() REPLACES the field; it refuses (returns false, field unchanged) anything that is not
    // a well-formed blob, since a save file is untrusted input.
    std::vector<uint8_t> serialize() const;
    bool                 deserialize(const uint8_t* data, size_t size);
    // The healing of `seconds` that passed while the field was not ticking (a save loaded a day later, a tile streamed
    // back in): the same rates as tick(), applied in one go with the CURRENT weather, capped at one game day. The weather in
    // between is unknown: this is an approximation and says so (docs/procedural-snow-sand-trails-plan section 3.4).
    void catchUp(double seconds, const TrailWeather& weather, const TrailSettings& settings = {});
    double clock() const { return m_clock; }
    void   setClock(double time) { m_clock = time; m_clockStarted = true; }

    size_t pageCount() const { return m_pages.size(); }
    // The texels that are not zero over all pages (a cheap "is anything trodden" for tests and tools).
    size_t activeTexels() const;
    // Forgets every trail; consumers that show them are told (the pages count as freed at the new version).
    void   clear();

private:
    struct Page
    {
        std::vector<int16_t>  depth = std::vector<int16_t>(static_cast<size_t>(kPageCells) * kPageCells, 0);
        uint8_t               surfaces = 0;   // bit set of TrailSurface
        uint64_t              version  = 0;   // the field's version at this page's last change
        std::array<uint64_t, kTilesPerSide * kTilesPerSide> tileVersion{};   // ... and at each tile's
        float                 carry = 0.0f;   // heal the last steps could not apply yet, in texel units
    };
    static int64_t key(int px, int pz) { return (static_cast<int64_t>(px) << 32) ^ static_cast<uint32_t>(pz); }

    int16_t* texelForWrite(int ix, int iz, TrailSurface surface);    // allocates the page
    int16_t  texel(int ix, int iz) const;                            // 0 off the pages
    float     healRate(uint8_t surfaces, const TrailWeather& w, const TrailSettings& s) const;
    bool      healPage(Page& page, float rate, float dt, uint64_t newVersion);   // true when it changed a texel

    std::unordered_map<int64_t, Page> m_pages;
    // Pages that healed away keep their tile versions: a mesh or texture that still shows their pits is told.
    std::unordered_map<int64_t, std::array<uint64_t, kTilesPerSide * kTilesPerSide>> m_freed;
    bool     m_clockStarted = false;
    double   m_clock        = 0.0;
    uint64_t m_version      = 0;
};
} // namespace HE
