#pragma once
#include <entt/entt.hpp>
#include <Types/UUID.h>
#include <cstdint>

// Marks a runtime-generated water surface: the entity that draws ONE body of a
// landscape's water (HE::water::Field, WaterField.h). Like a terrain chunk it is a
// child of its landscape, NEVER serialized and HIDDEN from the Outliner — WaterSurface
// recreates it from the TerrainComponent's water field, which is what is saved.
// isTerrainGenerated (TerrainChunkComponent.h) answers "is this one of those" for
// every place that has to leave such an entity alone.
//
// The entity carries an ordinary MeshComponent + MaterialComponent (the engine's
// water material), so the surface is drawn by the generic mesh path of every
// backend; nothing renderer-side knows about water.
struct WaterSurfaceComponent {
    entt::entity terrain = entt::null;   // the owning landscape entity
    uint16_t     body    = 0;            // HE::water::Body::id this surface draws
    // Not a surface but a SPRAY emitter (WaterSpray.h): the particle system of one place where fast water meets calm
    // water. Same family as a surface (a generated child of its landscape, never saved, hidden from the Outliner),
    // which is why it carries this component; WaterSurface::update keeps both. `sprayKey` says which foot it is
    // (the foot's position on a 2 m grid), `sprayStrength` the strength it was configured for.
    bool         spray = false;
    uint64_t     sprayKey = 0;
    float        sprayStrength = 0.0f;
    // The procedural mesh this entity draws and OWNS: registered once and afterwards
    // only replaced (every registration can move the content manager's pool under
    // other holders' pointers — the TerrainChunkComponent rule), given back when
    // the entity goes.
    HE::UUID     meshId{};

    // What the mesh was built from, so a later tick can tell "this body changed"
    // from "an edit happened nearby": a hash of the corner lattice and the build
    // options (WaterMesh.h surfaceHash), plus the figures the dirty test needs.
    uint64_t     hash = 0;
    // The inputs that are the same for every body of a landscape (its size,
    // resolution and world position, the UV scale). A change rebuilds all of them.
    uint64_t     paramsKey = 0;
    float        level = 0.0f;           // terrain-local Y the entity sits at
    int          cellX0 = 0, cellZ0 = 0, cellX1 = -1, cellZ1 = -1;   // the body's cell box
    uint32_t     triangles = 0;
    // The deepest water under the mesh when it was built (WaterMesh.h SurfaceMesh::deepest; -1 unknown):
    // the water material's Flow.z, so a camera below a pool it is not in does not see
    // the underside (Thema 232 Schritt 13).
    float        deepest = -1.0f;
};
