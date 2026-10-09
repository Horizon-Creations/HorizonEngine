#pragma once
#include <cstdint>

class HorizonWorld;
class ContentManager;
class IRenderer;

// ─── The water surface in the world ──────────────────────────────────────────
// Keeps one entity per water body of every landscape: a child of the landscape
// with an ordinary MeshComponent (the body's surface, WaterMesh.h) and an ordinary
// MaterialComponent (the engine's water material, Engine/Materials/Water.hasset).
// That is all there is to it on the renderer side — no water-specific draw path,
// so the surface goes through the generic translucent mesh path of Metal, OpenGL,
// D3D11, D3D12 and Vulkan alike.
//
// WHEN IT RUNS. TerrainSystem::updateTerrains calls it, so it runs in the world tick
// (SceneSystems::tickWorld) AND at every one of the editor's direct updateTerrains
// calls, including the headless dump that renders before any tick. A tick in which
// nothing changed costs a few comparisons per surface.
//
// WHAT IT REBUILDS. A water edit sets Field::dirty and the terrain-local rectangle
// it touched. Only the bodies that rectangle (grown by a cell) meets — by where
// their water is now or was when last built — are looked at, and of those only the
// ones whose corner lattice actually changed get a new mesh: a brush stroke at one
// end of a lake leaves a pond at the other, and a lake at a different level in the
// same rectangle, untouched. Changing a body's level, the terrain's size or
// resolution, or the landscape's world position (the UVs are world coordinates)
// rebuilds every body. Nothing here touches the terrain's chunks.
//
// WHAT IT CLEARS. Field::dirty, once it has caught up. Shore clipping against the
// ground (Schritt 5) adds a second input to the same pass: the ground's own dirty
// flags are read in TerrainSystem::updateTerrains right next to the call.
//
// OWNERSHIP. The meshes are registered once per surface and afterwards replaced in
// place (the pool of the content manager may move under other holders' pointers on
// every registration — the terrain-chunk rule). A mesh whose entity is gone, which is
// what an undo does to every generated entity at once, is given back on the next
// call.
namespace WaterSurface
{
    // Content path of the material every surface draws with (kEngineWaterMaterialId).
    inline constexpr const char* kMaterialPath = "Engine/Materials/Water.hasset";

    // Metres per UV unit. UVs are world coordinates / this, so 1 = one tile per metre.
    inline constexpr float kUvMetersPerTile = 1.0f;

    struct Stats
    {
        uint32_t terrains  = 0;   // landscapes with water, looked at
        uint32_t created   = 0;   // new surface entities
        uint32_t rebuilt   = 0;   // meshes replaced (created ones included)
        uint32_t kept      = 0;   // bodies examined and found unchanged
        uint32_t removed   = 0;   // surface entities taken away
        uint32_t triangles = 0;   // in the meshes built by this call
        bool     worked() const { return created || rebuilt || removed; }
    };

    // Bring the surfaces of every landscape in `world` in line with its water
    // field. `renderer` is nullable (no GPU cache to invalidate).
    void update(HorizonWorld& world, ContentManager& cm, IRenderer* renderer);

    // What the last call did. Diagnostics and tests; not a state to branch on.
    const Stats& lastStats();
}
