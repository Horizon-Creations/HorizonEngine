#pragma once
#include <HorizonScene/WaterField.h>
#include <glm/vec2.hpp>
#include <vector>

struct TerrainComponent;

// ─── Falls and the spray where they land (Thema 232) ─────────────────────────
// A stream runs at the speed its ground asks for (WaterMesh.h streamFlowAt). Where the ground is so steep that the water
// no longer lies on it but FALLS, the top of the face throws a curtain of falling particles, and where that curtain lands
// the water throws spray (drops, mist). Where a fast stream runs into calm water without a fall (a long steep slide into a
// pool) it throws spray too, at the line where it meets the calm water. Pure CPU like the rest of the water model: the
// landscape in, the falls and feet out; WaterSurface.cpp turns them into particle emitters (generated children of the
// landscape, never saved).
namespace HE::water
{
    inline constexpr float kSprayMinSpeed  = 1.5f;    // m/s: water slower than this does not throw spray
    inline constexpr float kSprayCalmRatio = 0.35f;   // downstream speed at most this share of the arriving speed
    inline constexpr float kSprayMinDrop   = 0.8f;    // m: a fall lower than this is a ripple
    inline constexpr float kSprayFullDrop  = 6.0f;    // m: from this fall on the spray is at full strength (with the speed)

    struct SprayFoot
    {
        glm::vec2 pos{ 0.0f };      // terrain-local XZ of the middle of the foot
        glm::vec2 dir{ 1.0f, 0.0f };// terrain-local unit direction the water arrives in
        float     surfaceY = 0.0f;  // terrain-local Y of the water there (the ground where there is none)
        float     speed = 0.0f;     // m/s of the water arriving
        float     drop = 0.0f;      // m it fell on the way
        float     strength = 0.0f;  // 0..1, sprayStrength(drop, speed)
        float     halfWidth = 0.0f; // m across the arriving water from the middle to the end of the foot
    };

    // ── Falls ───────────────────────────────────────────────────────────────────
    // One fall is one FACE: the stream's water where the ground ALONG the current is steeper than kFallSlope (what makes
    // water fall; the banks of its own channel do not), as one connected piece however it curves and however wide it is.
    // Its curtain leaves from the face's TOP edge — for every kFallLipSpacing of the face's width, the uppermost place
    // where the ground turns steeper than kFallSlope (a lip) — and falls down the face to where the ground along the
    // current eases off below kFallSlope again: there it lands, and there its spray rises. A face with a shelf in it (the
    // ground goes flat across the water and drops again) is two falls, one landing on the shelf, one from its edge.
    inline constexpr float kFallSlope      = 1.2f;    // m/m along the current from which the water falls, about 50 degrees
    inline constexpr float kFallMinHeight  = 1.5f;    // m: a steep run lower than this is a ledge, not a fall
    inline constexpr float kFallFullHeight = 12.0f;   // m: from this height the curtain is at full strength
    inline constexpr float kFallLipSpacing = 0.75f;   // m between two lips (the water grid's cell where that is coarser)

    struct FallLip
    {
        glm::vec2 pos{ 0.0f };       // terrain-local XZ of the lip, on the edge itself
        glm::vec2 dir{ 1.0f, 0.0f }; // terrain-local unit direction the water leaves in
        float     surfaceY = 0.0f;   // terrain-local Y of the water there
        float     speed = 0.0f;      // m/s it leaves at
        float     height = 0.0f;     // m from the edge down to where the face eases off
        float     strength = 0.0f;   // 0..1
        glm::vec2 landing{ 0.0f };   // terrain-local XZ where the face eases off below the lip (under kFallSlope)
        uint32_t  face = 0;          // which face it is the top of (findFallLips: the tallest face is 0)
    };
    float fallStrength(float height);
    // The top edge of every face in the landscape's streams, one lip per kFallLipSpacing of its width, the tallest first.
    std::vector<FallLip> findFallLips(const TerrainComponent& tc);

    // One straight piece of a face's top edge: its lips as ONE line, so the curtain starts flush, at one height. A face
    // whose edge bends, steps or lands in different places along its width (a lip further than kFallLineTolerance off the
    // line along the current or in height, or landing more than kFallLandTolerance elsewhere) is several pieces side by
    // side; an edge with no lip for kFallLineGap across the current breaks there.
    inline constexpr float kFallFreeSlope     = 2.5f;   // m/m: a face this steep throws all its water clear (a cliff)
    inline constexpr float kFallLineGap       = 3.2f;   // m across the current without a lip: two edges
    inline constexpr float kFallLineTolerance = 0.5f;   // m a lip may lie off its piece's line (along the current, in height)
    inline constexpr float kFallLandTolerance = 1.5f;   // m its landing may lie off the piece's (along, in height)
    inline constexpr size_t kFallMaxLines     = 32;     // per landscape: the tallest ones
    struct FallLine
    {
        glm::vec2 center{ 0.0f };      // terrain-local XZ, the middle of the edge
        glm::vec2 dir{ 1.0f, 0.0f };   // terrain-local unit direction the water leaves in (mean of the lips')
        glm::vec2 across{ 0.0f, 1.0f };// unit, perpendicular to dir
        float     halfWidth = 0.0f;    // m from the center to the end of the edge (the outer lips' half spacing included)
        float     surfaceY = 0.0f;     // terrain-local Y of the water at the edge (mean)
        float     speed = 0.0f;        // m/s it leaves at (mean)
        float     height = 0.0f;       // m it falls (mean of the lips')
        float     strength = 0.0f;     // 0..1, fallStrength(height)
        uint32_t  lips = 0;            // how many lips it stands for
        // How steep the face is as a whole, m/m (height over its run down the face): about kFallSlope for a rounded wall
        // the water mostly rides down, kFallFreeSlope and more for a cliff it is thrown clear of.
        float     slope = 0.0f;
        // Where its curtain lands: `run` metres on along dir from the edge — down the face to its foot, or as far as the
        // water leaving at `speed` carries in the fall's time, whichever is further — at landingY (the water there, or the
        // ground). The curtain is thrown to land exactly there, and the spray of this fall rises exactly there.
        float     run = 0.0f;
        glm::vec2 landing{ 0.0f };
        float     landingY = 0.0f;
    };
    // The pieces of every face's edge in the landscape's streams, tallest first, at most kFallMaxLines. A piece whose edge
    // lies under the curtain of a higher piece (between its edge and where it lands) is that fall, not one of its own.
    std::vector<FallLine> findFallLines(const TerrainComponent& tc);

    // A fall: one piece of edge and the foot its curtain lands on (as wide as the piece, at FallLine::landing).
    struct Fall
    {
        FallLine  line;
        SprayFoot foot;
    };
    std::vector<Fall> findFalls(const TerrainComponent& tc);

    // 0 below kSprayMinDrop or kSprayMinSpeed, then rising with the fall (to kSprayFullDrop) and the speed (to the
    // fastest a stream runs, kStreamMaxSpeed), 1 at most.
    float sprayStrength(float drop, float speed);

    // Where fast water (>= kSprayMinSpeed) runs into calm water (<= kSprayCalmRatio of its speed) after a drop of at least
    // kFallMinHeight without falling: one foot per such line across the water, as wide as it. None where a fall of `falls`
    // already lands (its foot is that spray).
    std::vector<SprayFoot> findRunoutFeet(const TerrainComponent& tc, const std::vector<Fall>& falls);

    // Every foot in the landscape's streams: the falls' (findFalls) and the run-outs' (findRunoutFeet), strongest first.
    // Empty for a landscape with no stream.
    std::vector<SprayFoot> findSprayFeet(const TerrainComponent& tc);
}
