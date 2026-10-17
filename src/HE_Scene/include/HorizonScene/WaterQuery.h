#pragma once
#include <HorizonScene/WaterField.h>
#include <Physics/WaterWaves.h>
#include <Math/Math.h>
#include <entt/entt.hpp>
#include <cstdint>
#include <vector>

class HorizonWorld;
struct TerrainComponent;

// ─── "Where is the water, and what is it doing here" (Thema 179 Schritt 2) ───
// The question the water physics asks of a landscape, and gameplay with it: for a
// WORLD position, is there water, how high is its surface (waves included), how does
// it tilt, how deep is it, which way does it flow. Pure CPU, no Jolt, no renderer, no
// ContentManager — a landscape on the stack answers it, the way WaterField does.
//
// WHERE THE ANSWER COMES FROM. From the FIELD (TerrainComponent::water), not from the
// water surface entities: those are generated children that may be a frame behind an
// edit, hold only what survived the shore clip, and exist only where a renderer-side
// system ran. The field is the intent; the same two things the mesh builder checks
// (the cell says "water", and the ground does not stand above the level plus the
// overshoot) are checked here, so a body is wet exactly where the sheet is drawn.
//
// THE WAVES are Physics/WaterWaves.h's — the numbers and the formula the engine water
// material is built from — evaluated at the simulation time the caller passes. Phase and
// time are the physics' own, so the surface a crate floats on has the heights and the
// directions of the water you see, not its phase (that file says why).
//
// SPACE. The caller speaks WORLD. A landscape is composed with HE::worldMatrixOf when
// the index is refreshed (never TransformComponent::worldMatrix, which is a frame
// stale), and a point is taken into the landscape's local XZ by that matrix. A landscape
// may be moved, turned around Y and scaled; one tilted around X or Z is not supported
// (the surface is level in world Y, like the shader assumes).
namespace HE::water
{
    // The answer for one world position. Everything is in world space except `physics`.
    struct Surface
    {
        bool         wet     = false;
        entt::entity terrain = entt::null;   // the landscape the water belongs to
        uint16_t     body    = kNoBody;      // which body of water
        float        level   = 0.0f;         // the surface without waves (world Y)
        float        height  = 0.0f;         // …and with them (world Y)
        glm::vec3    normal  { 0.0f, 1.0f, 0.0f };   // of the wave surface
        float        depth   = 0.0f;         // height above the ground, never negative
        glm::vec3    flow    { 0.0f };       // the current, m/s
        BodyPhysics  physics {};             // that body's settings (density, drags, …)
        UnderwaterFogParams underwaterFog{}; // that body's look from below (Thema 185 Schritt 2)
        UnderwaterCausticsParams underwaterCaustics{};   // …and its refracted-light net (Schritt 3)
    };

    // A box in world space where there is water (for the physics' broad phase).
    struct Region
    {
        entt::entity terrain = entt::null;
        glm::vec3    min{ 0.0f }, max{ 0.0f };
    };

    class WaterIndex
    {
    public:
        // Bring the snapshot in line with the world: every landscape with at least one
        // body of water and an allocated grid. Cheap when nothing moved — the wet
        // rectangle of a landscape is only rescanned when its Field::revision (or its
        // resolution, or the grid itself) changed. Call it once per physics step, and
        // before a batch of queries; the index keeps pointers to the landscapes'
        // components, so a component added or removed afterwards invalidates it.
        void refresh(HorizonWorld& world);

        // A fresh snapshot, for one-off questions (a script asking for the height).
        static WaterIndex build(HorizonWorld& world);

        // No landscape has water: the whole price of a scene without it.
        bool empty() const { return m_entries.empty(); }
        size_t landscapes() const { return m_entries.size(); }

        // Water at world (x, z) at simulation time t. False (out untouched apart from
        // `wet = false`) when it is dry there. Several landscapes: the highest surface
        // wins, a tie goes to the smaller entity id (never a hash order).
        bool surfaceAt(float x, float z, double t, Surface& out) const;

        // Just the height, NaN when dry.
        float heightAt(float x, float z, double t) const;

        // The world boxes where a body can be in water: per landscape, the wet cells'
        // rectangle, from the lowest water level (minus kRegionDepth) up to the highest
        // level plus the waves and a metre. Landscapes without a wet cell give none.
        // Non-const because it rescans a landscape whose revision moved.
        void regions(std::vector<Region>& out);

        // Revision of the field last seen for a landscape (0 = unknown); lets the
        // physics notice that a landscape's water was edited.
        uint64_t revisionOf(entt::entity terrain) const;

        static constexpr float kRegionDepth = 512.0f;   // metres below the lowest level

    private:
        struct Entry
        {
            entt::entity            entity = entt::null;
            const TerrainComponent* tc     = nullptr;
            glm::mat4               localToWorld{ 1.0f };
            glm::mat4               worldToLocal{ 1.0f };
            glm::mat3               rotation{ 1.0f };   // localToWorld's turn, scale removed (for the current)
            // The wet cells' rectangle in terrain-local XZ, cached against the key.
            bool      boundsValid = false;
            bool      anyWet      = false;
            float     minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
            float     minLevel = 0.0f, maxLevel = 0.0f;   // over the bodies that own wet cells
            uint64_t  keyRevision = 0;
            uint32_t  keyRes      = 0;
            const void* keyGrid   = nullptr;
            size_t    keyBodies   = 0;
        };
        bool entryAt(const Entry& e, float x, float z, double t, Surface& out) const;
        void ensureBounds(Entry& e);

        std::vector<Entry> m_entries;   // sorted by entity id
    };
}
