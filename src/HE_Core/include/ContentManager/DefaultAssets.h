#pragma once
#include "Types/UUID.h"

// Well-known UUIDs for the engine's built-in default assets.  These are
// registered in every ContentManager instance at construction time (see
// ContentManager::initDefaultAssets) so that renderers and procedural systems
// can look them up by UUID without any path-based loading.
//
// The hi/lo values are chosen to never collide with UUID::generate() output:
// generate() enforces version-4 bits (hi & 0xF000 == 0x4000) whereas these
// sentinels have hi < 0x10, which no RNG will ever produce.

namespace HE {

// A unit cube (24 verts: pos3 + normal3 per face, 36 indices).
// Used as the fallback mesh for entities that have no MeshComponent asset.
constexpr UUID kDefaultCubeMeshId     = { 0x0000000000000001ULL, 0x0000000000000001ULL };

// A 1×1 RGBA8 white pixel texture.
// Useful as a "no texture" placeholder or neutral multiplier.
constexpr UUID kDefaultWhiteTextureId = { 0x0000000000000002ULL, 0x0000000000000001ULL };

// A material with default PBR scalars (baseColor white, metallic 0, roughness 0.5).
// Useful as a starting point for newly created materials.
constexpr UUID kDefaultMaterialId     = { 0x0000000000000003ULL, 0x0000000000000001ULL };

} // namespace HE
