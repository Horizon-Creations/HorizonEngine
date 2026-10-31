#pragma once
#include "HorizonScene/WaterField.h"

#include <cstdint>
#include <string>
#include <vector>

namespace HE
{

// ── The bodies of water of a tile landscape (Thema 326, step 3c) ─────────────────
// A tile stores, per cell, how much of it is water and WHICH body owns it (LandscapeTile::waterOwner, a u16). A lake across four tiles must
// be one body with one level, and the single landscape's ids are per terrain, so the ids of a tile landscape are global: this table, in the
// pack, says what each id is. Id 0 is "no body". Only what the water needs to be a lake is here: id, level, physics, the look from below and
// the tessellation settings. The editor state of a water::Body (its polygon, the bed it dug, the spline it came from) is not part of a pack.
//
// `level` is the surface in ABSOLUTE metres. The terrain a tile (or a region of tiles) is turned into sits at y = 0 with absolute heights
// (LandscapeTileTerrain.h), so a body's terrain-local level and its absolute level are the same number.
struct LandscapeWaterBody
{
	uint16_t                          id = 0;
	float                             level = 0.0f;
	water::BodyPhysics                physics;
	water::UnderwaterFogParams        underwaterFog;
	water::UnderwaterCausticsParams   underwaterCaustics;
	water::TessellationParams         tessellation;
};

struct LandscapeWaterTable
{
	std::vector<LandscapeWaterBody> bodies;
	// The landscape-wide settings of water::Field.
	bool  clipToGround = true;
	float shoreOvershoot = water::kDefaultShoreOvershoot;
	bool  tessellation = false;

	const LandscapeWaterBody* find(uint16_t id) const
	{
		for (const LandscapeWaterBody& b : bodies)
			if (b.id == id) return &b;
		return nullptr;
	}
};

// Little endian:
//   u32 magic 'HLW1' (0x31574c48)  u32 version (1)  u32 byteSize (of the whole table, this header included)
//   u8 clipToGround  f32 shoreOvershoot  u8 tessellation  u32 count
//   per body: u16 id  f32 level
//             u8 physics.enabled  f32 density  f32 linearDrag  f32 angularDrag  f32 flow.x  f32 flow.y  f32 splash
//             f32 fog.shallowColor[3]  f32 fog.deepColor[3]  f32 fog.density  f32 fog.falloff
//             f32 caustics.intensity  f32 caustics.scale  f32 caustics.speed  f32 caustics.depth
//             f32 tessellation.height  f32 tessellation.shoreWidth                                  -> 87 bytes a body, 22 before the first
// A reader refuses a wrong magic or version, a byteSize that does not match the blob, a count the blob cannot hold, an id of 0 or twice,
// and a level that is not a number; the settings are held to their bounds (water::clamp...).
constexpr uint32_t kLandscapeWaterMagic = 0x31574c48u;
constexpr uint32_t kLandscapeWaterVersion = 1;
constexpr size_t   kLandscapeWaterHeaderBytes = 12;
constexpr size_t   kLandscapeWaterBodyBytes = 87;

std::vector<uint8_t> encodeLandscapeWaterTable(const LandscapeWaterTable& table);
bool decodeLandscapeWaterTable(const uint8_t* data, size_t size, LandscapeWaterTable& out, std::string* error = nullptr);

} // namespace HE
