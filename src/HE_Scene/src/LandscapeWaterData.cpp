#include "HorizonScene/LandscapeWaterData.h"

#include <cmath>
#include <cstring>

namespace HE
{
namespace
{
void putU16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(static_cast<uint8_t>(v)); o.push_back(static_cast<uint8_t>(v >> 8)); }
void putU32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void putF32(std::vector<uint8_t>& o, float f) { uint32_t b; std::memcpy(&b, &f, 4); putU32(o, b); }

struct Reader
{
	const uint8_t* p;
	size_t n, pos = 0;
	bool ok = true;
	uint8_t u8() { if (pos + 1 > n) { ok = false; return 0; } return p[pos++]; }
	uint16_t u16() { if (pos + 2 > n) { ok = false; return 0; } const uint16_t v = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8)); pos += 2; return v; }
	uint32_t u32() { if (pos + 4 > n) { ok = false; return 0; } const uint32_t v = p[pos] | (p[pos + 1] << 8) | (p[pos + 2] << 16) | (static_cast<uint32_t>(p[pos + 3]) << 24); pos += 4; return v; }
	float f32() { const uint32_t b = u32(); float f; std::memcpy(&f, &b, 4); return f; }
};

bool fail(std::string* e, const char* why) { if (e) *e = why; return false; }
} // namespace

std::vector<uint8_t> encodeLandscapeWaterTable(const LandscapeWaterTable& t)
{
	std::vector<uint8_t> o;
	putU32(o, kLandscapeWaterMagic);
	putU32(o, kLandscapeWaterVersion);
	putU32(o, 0);   // byteSize, patched below
	o.push_back(t.clipToGround ? 1 : 0);
	putF32(o, t.shoreOvershoot);
	o.push_back(t.tessellation ? 1 : 0);
	putU32(o, static_cast<uint32_t>(t.bodies.size()));
	for (const LandscapeWaterBody& b : t.bodies)
	{
		putU16(o, b.id);
		putF32(o, b.level);
		o.push_back(b.physics.enabled ? 1 : 0);
		putF32(o, b.physics.density);
		putF32(o, b.physics.linearDrag);
		putF32(o, b.physics.angularDrag);
		putF32(o, b.physics.flow.x);
		putF32(o, b.physics.flow.y);
		putF32(o, b.physics.splash);
		for (int i = 0; i < 3; ++i) putF32(o, b.underwaterFog.shallowColor[i]);
		for (int i = 0; i < 3; ++i) putF32(o, b.underwaterFog.deepColor[i]);
		putF32(o, b.underwaterFog.density);
		putF32(o, b.underwaterFog.falloff);
		putF32(o, b.underwaterCaustics.intensity);
		putF32(o, b.underwaterCaustics.scale);
		putF32(o, b.underwaterCaustics.speed);
		putF32(o, b.underwaterCaustics.depth);
		putF32(o, b.tessellation.height);
		putF32(o, b.tessellation.shoreWidth);
	}
	const uint32_t size = static_cast<uint32_t>(o.size());
	for (int i = 0; i < 4; ++i) o[8 + static_cast<size_t>(i)] = static_cast<uint8_t>(size >> (8 * i));
	return o;
}

bool decodeLandscapeWaterTable(const uint8_t* data, size_t size, LandscapeWaterTable& out, std::string* error)
{
	out = LandscapeWaterTable{};
	if (size < kLandscapeWaterHeaderBytes) return fail(error, "water table shorter than its header");
	Reader r{ data, size };
	if (r.u32() != kLandscapeWaterMagic) return fail(error, "not a water table (bad magic)");
	if (r.u32() != kLandscapeWaterVersion) return fail(error, "unknown water table version");
	if (r.u32() != size) return fail(error, "water table size does not match its blob");
	out.clipToGround = r.u8() != 0;
	out.shoreOvershoot = r.f32();
	out.tessellation = r.u8() != 0;
	const uint32_t count = r.u32();
	if (!r.ok) return fail(error, "water table is cut off");
	if (!std::isfinite(out.shoreOvershoot)) out.shoreOvershoot = water::kDefaultShoreOvershoot;
	out.shoreOvershoot = std::min(std::max(out.shoreOvershoot, 0.0f), water::kMaxShoreOvershoot);
	if (count > 65535u || size - r.pos < static_cast<size_t>(count) * kLandscapeWaterBodyBytes) return fail(error, "water table claims more bodies than it holds");
	out.bodies.reserve(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		LandscapeWaterBody b;
		b.id = r.u16();
		b.level = r.f32();
		b.physics.enabled = r.u8() != 0;
		b.physics.density = r.f32();
		b.physics.linearDrag = r.f32();
		b.physics.angularDrag = r.f32();
		b.physics.flow.x = r.f32();
		b.physics.flow.y = r.f32();
		b.physics.splash = r.f32();
		for (int k = 0; k < 3; ++k) b.underwaterFog.shallowColor[k] = r.f32();
		for (int k = 0; k < 3; ++k) b.underwaterFog.deepColor[k] = r.f32();
		b.underwaterFog.density = r.f32();
		b.underwaterFog.falloff = r.f32();
		b.underwaterCaustics.intensity = r.f32();
		b.underwaterCaustics.scale = r.f32();
		b.underwaterCaustics.speed = r.f32();
		b.underwaterCaustics.depth = r.f32();
		b.tessellation.height = r.f32();
		b.tessellation.shoreWidth = r.f32();
		if (!r.ok) return fail(error, "water table is cut off");
		if (b.id == water::kNoBody) return fail(error, "water table has a body with id 0");
		if (!std::isfinite(b.level)) return fail(error, "water table has a body whose level is not a number");
		if (out.find(b.id)) return fail(error, "water table has a body id twice");
		b.physics = water::clampPhysics(b.physics);
		b.underwaterFog = water::clampUnderwaterFog(b.underwaterFog);
		b.underwaterCaustics = water::clampUnderwaterCaustics(b.underwaterCaustics);
		b.tessellation = water::clampTessellation(b.tessellation);
		out.bodies.push_back(b);
	}
	return true;
}

} // namespace HE
