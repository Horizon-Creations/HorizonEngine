#pragma once
#include <Renderer/WaterRipples.h>   // WaterRippleBlock, WaterRippleKind (HE_Core)
#include <glm/glm.hpp>

class IRenderer;
class PhysicsWorld;

// ─── The rings on the water (Thema 179 Schritt 3) ─────────────────────────────
// A short, bounded list of disturbances of the water surface: where a body went
// into or out of it (or moved through it, or a script asked for one), how big the
// ring starts, how strong, how old. The water physics fills it
// (PhysicsWorld::applyWater), the application loop packs it into a WaterRippleBlock
// every frame (pack → IRenderer::SetWaterRipples), and the engine water material
// draws the rings from that block (the Water Ripple node). Display only: nothing
// reads a ring back into the simulation, so it adds no state to the physics and no
// question to the network (docs/water-physics-plan.md §6.1, §8.2).
//
// BOUNDED, ON PURPOSE. kMax entries; adding to a full list replaces the weakest, and
// only when the newcomer is stronger — a splash that is weaker than everything on show
// is not worth losing a ring for. A ring fades with age (strength * exp(-age / decay))
// and leaves the list when it is below kMinStrength; a ring raised on top of a young one
// of the same kind within kMergeDistance refreshes it instead of adding another, so a
// heap of two hundred crates falling at the same spot does not fill the list (a trail
// never merges: a body moving through the water leaves a line of rings, one per
// kTrailInterval of its own). The shader's loop is as long as the list is, so this bound
// is its cost bound too.
//
// TIME is the simulation's: age grows by the dt of the physics step (advance), never by
// the render clock, so a paused or slowed game keeps its rings in step with its bodies.
//
// Pure CPU, no Jolt, no renderer — a test puts rings in and reads the block out.
namespace HE::water
{
    struct Ripple
    {
        glm::vec2 pos{ 0.0f };      // centre, world XZ
        float     radius   = 0.3f;  // the ring starts at this radius (m)
        float     strength = 0.0f;  // 0..1 at age 0
        float     age      = 0.0f;  // seconds since it was raised
        glm::vec2 drift{ 0.0f };    // m/s the centre moves (the current carries a ring)
        int       kind     = WaterRippleImpact;
    };

    // When the water physics raises a ring (PhysicsWorld::applyWater), kept here so the
    // tests can put a body on either side of each line. The share is the part of a body's
    // volume under water; the speeds are relative to the water's surface and its current.
    struct RippleTriggers
    {
        static constexpr float kWetShare        = 0.02f;   // under this share a body is out of the water
        static constexpr float kImpactSpeed     = 1.0f;    // m/s into the surface for a body going in
        static constexpr float kExitSpeed       = 1.5f;    // m/s out of it for a body coming out
        static constexpr float kTrailSpeed      = 0.6f;    // m/s through the water for a body cutting the surface
        static constexpr float kTrailMaxShare   = 0.98f;   // a body this far under is not on the surface
        static constexpr float kTrailInterval   = 0.12f;   // s of simulation between one body's trail rings
        static constexpr int   kMaxImpactsPerStep = 4;     // entries and exits raised in one step
        static constexpr int   kMaxTrailsPerStep  = 2;     // trail rings raised in one step
    };

    class RippleField
    {
    public:
        static constexpr int   kMax           = kWaterRippleMax;
        // How the shader draws a ring, handed over in the block head: the front moves
        // out at this speed, the strength decays with this time constant.
        static constexpr float kRingSpeed     = 1.1f;    // m/s
        static constexpr float kDecayTime     = 1.4f;    // s
        // A ring weaker than this (after fading) is gone.
        static constexpr float kMinStrength   = 0.02f;
        // A new ring this close to a young one of the same kind refreshes it.
        static constexpr float kMergeDistance = 0.3f;    // m
        static constexpr float kMergeAge      = 0.25f;   // s

        // Raise a ring. Strength is clamped to 0..1, radius to a sensible range; a ring
        // that is not a finite number, or has no strength, is ignored. See the header.
        void add(const Ripple& r);

        // Age every ring by dt (seconds of simulation), carry it along its drift and
        // drop the ones that faded out. The order of the survivors is kept.
        void advance(float dt);

        void clear() { m_count = 0; }
        int  count() const { return m_count; }
        const Ripple& at(int i) const { return m_rings[i]; }

        // The strength a ring has now: strength * exp(-age / decay).
        static float effectiveStrength(const Ripple& r);

        // The block the shaders read (Renderer/WaterRipples.h): count and the ring
        // constants in head, then one entry per ring in list order.
        void pack(WaterRippleBlock& out) const;
        WaterRippleBlock pack() const { WaterRippleBlock b; pack(b); return b; }

    private:
        Ripple m_rings[kMax];
        int    m_count = 0;
    };

    // The frame's push, in one place for the packaged game and the editor's play mode:
    // pack the rings of `physics` (none for nullptr) and hand the block to the renderer.
    // Call it once per frame next to the environment push, after the physics steps of
    // the frame — a frame that never calls it draws the water as it always did.
    void pushRipples(IRenderer* renderer, const PhysicsWorld* physics);
}
