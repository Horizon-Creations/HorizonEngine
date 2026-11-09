#pragma once
#include <ContentManager/DefaultAssets.h>
#include <Types/UUID.h>

// ─── Water obstacle (Thema 232) ─────────────────────────────────────────────
// Marks an entity the landscape water treats as standing IN it: a rock in a river,
// a bridge pier, a post in a lake. Where the water runs past it (a stream's current,
// a lake given a flow), foam gathers along the entity's mesh bounds — a collar where
// the water meets it, a wake trailing downstream — the stronger the faster the water.
// Still water shows nothing. The footprint is the entity's mesh bounds under its
// world transform, and only while that box reaches through the water's surface.
//
// What it does NOT do: the water does not route around the entity — the water field,
// the meshes, the physics' flow and the spray are unchanged. Foam is drawn as a
// band over the water (HE::water::obstacleFoam, RenderExtractor), rebuilt every
// frame, so a moving obstacle (dragged, simulated) carries its foam with it. The
// band is drawn with `foamMaterial`, an ordinary material asset: the engine's own
// foam by default, any translucent material that reads the band (the Foam Band
// material node) instead.
struct WaterObstacleComponent
{
    bool  enabled = true;
    float foam    = 1.0f;   // strength, 0..4 (1 = the default look)
    float margin  = 0.6f;   // m the collar reaches out from the bounds, at the sides and upstream
    float wake    = 3.0f;   // m the foam trails downstream behind it, at full current
    HE::UUID foamMaterial = HE::kEngineWaterFoamMaterialId;   // the material the band is drawn with
};
