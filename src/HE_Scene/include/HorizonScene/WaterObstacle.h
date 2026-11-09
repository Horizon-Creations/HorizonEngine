#pragma once
#include <HorizonScene/WaterQuery.h>
#include <Math/AABB.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <cstdint>
#include <vector>

class ContentManager;
struct WaterObstacleComponent;

// ─── Foam around water obstacles (Thema 232) ─────────────────────────────────
// An entity with a WaterObstacleComponent standing in the landscape water: where the water runs past it, foam
// gathers along its mesh bounds — a collar where the water meets it and a wake trailing downstream, the stronger
// the faster the water; still water none. Pure CPU, in WORLD space through the water query (WaterIndex: streams,
// lakes, their flow alike), so a landscape that is moved, turned or scaled needs nothing extra. RenderExtractor
// builds the band every frame and hands it to the backends as a RibbonBatch (the trails' path) drawn with the
// component's foam material (an ordinary material asset; the engine's own by default).
namespace HE::water
{
    // The obstacle's footprint: its mesh bounds under its world transform, seen from above as a box — the centre,
    // the two most horizontal half axes (their XZ part) — and the vertical range the box covers.
    struct ObstacleFootprint
    {
        glm::vec2 center{ 0.0f };
        glm::vec2 halfA{ 0.5f, 0.0f };
        glm::vec2 halfB{ 0.0f, 0.5f };
        float     minY = 0.0f;
        float     maxY = 0.0f;
    };
    ObstacleFootprint obstacleFootprint(const glm::mat4& world, const float boundsMin[3], const float boundsMax[3]);

    inline constexpr float kObstacleMinSpeed  = 0.15f;  // m/s: slower water is still, no foam
    inline constexpr float kObstacleFullSpeed = 1.5f;   // m/s: from here on the foam is at full strength
    inline constexpr int   kObstacleFoamSegments = 48;  // around the footprint
    inline constexpr float kObstacleFoamLift = 0.04f;   // m over the water's surface (no fighting with the sheet)
    // The band's UV: fract(uv.x) = 0 at the obstacle .. 1 at the foam's outer edge; floor(uv.x) / kObstacleFoamLevels
    // = the strength (foam x current); uv.y = where the band faces, 1 straight into the current (the pillow) .. 0
    // straight downstream (the eddy, the wake). The normal leans the way the water runs at the vertex,
    // n.xz / n.y / kObstacleFoamFlowGain = that direction times the current's share of kObstacleFullSpeed: along
    // the obstacle's face close to it, the free current further out.
    // (kObstacleFoamLevels, kObstacleFoamFlowGain: Physics/WaterWaves.h, shared with the material.)

    struct FoamBand
    {
        std::vector<float>    vertices;   // pos3 + norm3 + uv2, world space
        std::vector<uint32_t> indices;
        HE::AABB              bounds;
        glm::vec2             flowDir{ 1.0f, 0.0f };   // world XZ, unit
        float                 speed = 0.0f;            // m/s of the water around it
        float                 strength = 0.0f;         // what floor(uv.x) carries, / kObstacleFoamLevels
        float                 level = 0.0f;            // world Y of the water at the obstacle
    };

    // The foam band of one obstacle, false when there is none: no water there, the box does not reach through the
    // surface (fully under or above it), the water is still, or the component is off or at zero foam.
    bool obstacleFoam(const WaterIndex& index, const ObstacleFootprint& fp, const WaterObstacleComponent& c, double t,
                      FoamBand& out);
}
