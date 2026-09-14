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

// A 128×128 RGBA8 grid texture: light cool-grey cells with single-pixel slate
// blue-grey lines every 32 pixels and a lighter accent dot at each cell corner.
// Tile-friendly. Registered for any system that wants a technical grid; nothing
// references it by default (the terrain material below is untextured).
constexpr UUID kDefaultGridTextureId  = { 0x0000000000000004ULL, 0x0000000000000001ULL };

// A flat neutral-grey, double-sided, rough material with no texture — keeps
// terrain readable without visual noise. Assigned to newly created Landscape
// entities and to every terrain chunk the TerrainSystem spawns.
constexpr UUID kDefaultTerrainMaterialId = { 0x0000000000000005ULL, 0x0000000000000001ULL };

// A 1×1 billboard quad in the XY plane, normal pointing +Z.
// Default mesh for ParticleSystemComponent; also useful for sprite quads.
constexpr UUID kDefaultQuadMeshId = { 0x0000000000000006ULL, 0x0000000000000001ULL };

// A flat 6-pointed star in the XY plane, normal +Z (radius ~0.5). Billboarded by the
// weather system as a snowflake so flakes read as a shape, not a white square.
constexpr UUID kDefaultSnowflakeMeshId = { 0x0000000000000007ULL, 0x0000000000000001ULL };

// A 1x1 RGBA8 (255, 0, 0, 0) texture: the "nothing painted yet" landscape weightmap.
// A Landscape Layer Blend node normalises by the weight sum, so an all-white default
// would average every layer and a black one would divide by ~0 — full weight on
// channel 0 makes an unpainted terrain show layer 0, which is what you want.
constexpr UUID kDefaultLayer0WeightTextureId = { 0x0000000000000008ULL, 0x0000000000000001ULL };

// ── Editor icons ─────────────────────────────────────────────────────────────
// The billboards the scene view draws for entities that have no geometry of
// their own — a light, a camera, an audio source — so they can be seen and
// clicked (RenderExtractor emits them only under an active editor camera, so
// a packaged game never draws one). 64×64 RGBA8 each, rasterized at
// construction from the engine's icon face (UIFont's Material Symbols): a
// white glyph with a dark halo, which reads on a bright sky and on a dark
// wall alike. Texture row 0 is the BOTTOM, the engine's texture convention.
constexpr UUID kEditorIconPointLightTextureId       = { 0x0000000000000009ULL, 0x0000000000000001ULL };
constexpr UUID kEditorIconSpotLightTextureId        = { 0x000000000000000AULL, 0x0000000000000001ULL };
constexpr UUID kEditorIconDirectionalLightTextureId = { 0x000000000000000BULL, 0x0000000000000001ULL };
constexpr UUID kEditorIconCameraTextureId           = { 0x000000000000000CULL, 0x0000000000000001ULL };
constexpr UUID kEditorIconAudioSourceTextureId      = { 0x000000000000000DULL, 0x0000000000000001ULL };

// One unlit, alpha-blended, double-sided material per icon texture above:
// an unlit Translucent node graph (Texture Sample → BaseColor + Opacity), so
// the icon keeps its colour at night and its transparent corners stay
// transparent. Authored as a graph rather than hand-written GLSL so it goes
// through exactly the codegen every project material goes through.
constexpr UUID kEditorIconPointLightMaterialId       = { 0x000000000000000EULL, 0x0000000000000001ULL };
constexpr UUID kEditorIconSpotLightMaterialId        = { 0x000000000000000FULL, 0x0000000000000001ULL };
constexpr UUID kEditorIconDirectionalLightMaterialId = { 0x0000000000000010ULL, 0x0000000000000001ULL };
constexpr UUID kEditorIconCameraMaterialId           = { 0x0000000000000011ULL, 0x0000000000000001ULL };
constexpr UUID kEditorIconAudioSourceMaterialId      = { 0x0000000000000012ULL, 0x0000000000000001ULL };

// Pixel size of the editor icon textures above.
constexpr int kEditorIconTextureSize = 64;

} // namespace HE
