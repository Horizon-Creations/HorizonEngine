#pragma once

// ─── WaterRippleBlock ────────────────────────────────────────────────────────
// The disturbances of the water surface (the rings a body raises where it goes into
// or out of the water) as the shaders read them. The HeLighting tail
// Lighting::waterRipples / waterRippleA / waterRippleB (MaterialShaderLibrary.h) is
// this block field for field, and HE::FillMaterialWater copies the three arrays over.
//
// Plain data in HE_Core so the scene can pack it (HE::water::RippleField::pack, in
// HE_Scene) and a renderer take it (IRenderer::SetWaterRipples) without either side
// knowing the other — the way EnvironmentSettings travels. Pushed every frame by the
// application loop; the Water Ripple graph node (heWaterRipple) is the reader.
//
// The rings are display only. Nothing reads them back into the simulation, so they
// add no state to the physics and no question to the network (docs/water-physics-plan.md
// §6.1, §8.2).
//
// Append-only like Lighting: the layout is the contract between the scene, the five
// backends and the node's shader text. A new field goes at the end, the three arrays
// keep their offsets and their stride of one vec4.
inline constexpr int kWaterRippleMax = 16;

struct WaterRippleBlock
{
    // x = live entries (0..kWaterRippleMax; 0 → the shader never enters its loop),
    // y = ring speed (m/s), z = decay time constant (s), w = reserved.
    float head[4] = {};
    // xy = centre (world XZ), z = the radius the ring starts from (m), w = strength 0..1.
    float a[kWaterRippleMax][4] = {};
    // x = age (s), y = kind (WaterRippleKind), zw = reserved.
    float b[kWaterRippleMax][4] = {};
};

// What raised a ring. The shader draws them the same way (the kind rides in b[i].y for
// a material that wants to tell them apart); the scene side uses it to decide which
// rings may be merged.
enum WaterRippleKind : int
{
    WaterRippleImpact = 0,   // a body went into the water
    WaterRippleTrail  = 1,   // a body moving through the surface
    WaterRippleScript = 2,   // asked for by a script (water.disturb)
    WaterRippleExit   = 3,   // a body came out of the water
};
