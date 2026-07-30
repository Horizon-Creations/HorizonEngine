#pragma once
#include <Renderer/IRenderer.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// ─── Shared GPU weather-particle seeding ─────────────────────────────────────
// The initial state of the rain/snow pool, previously duplicated character-for-
// character in OpenGLRenderer::SeedParticleBuffer and
// MetalRenderer::SeedParticleBuffer — same 0x9E3779B9 start value, same LCG,
// same (i+0.5)/count spread, same isSnow lane maths. Only the LANE ORDER of the
// 8 floats differed, because the two simulation shaders read the record
// differently; that is now the `layout` parameter.
//
// The generated values are observable: the simulation shaders do
// `step(seed, coverage)` against lane 7, so the even (i+0.5)/count spread is what
// makes exactly a `coverage` fraction of the pool live. Changing the RNG changes
// the look of a downpour on every backend at once.
//
// Header-only on purpose: it is a pure function over the params struct with no
// state to hide, and inlining it keeps it out of the HE_RENDERING_API surface.
namespace HE
{

// The two 8-float record layouts in use. Lanes 0..2 are always the world
// position and lane 7 is always the spawn seed; the two backends disagree only
// about where `life` sits relative to the velocity.
enum class WeatherParticleLayout
{
	PosVelLifeSeed, // OpenGL transform feedback: [px,py,pz, vx,vy,vz, life, seed]
	PosLifeVelSeed, // Metal compute kernel:      [px,py,pz, life, vx,vy,vz, seed]
};

// Floats per particle record. Both layouts are this wide; the backends' buffer
// allocations must agree with it.
inline constexpr int kWeatherParticleFloats = 8;

// Writes `count` records into `out` (which must hold count * kWeatherParticleFloats
// floats). Pre-distributes the pool down the fall column so it does not all spawn
// at once.
inline void SeedWeatherParticles(const IRenderer::GpuParticleParams& p,
                                 int                                 count,
                                 WeatherParticleLayout               layout,
                                 float*                              out)
{
	if (!out || count <= 0) return;
	const float top = p.cameraPos.y + p.boxTop;
	// Fixed start value: seeding must be reproducible across backends and runs —
	// this is the golden-ratio constant, not a time-derived seed.
	uint32_t rng = 0x9E3779B9u;
	auto frand = [&]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) * (1.0f / 16777216.0f); };
	for (int i = 0; i < count; ++i)
	{
		const float seed = (i + 0.5f) / static_cast<float>(count);
		const float y    = p.groundLevel + frand() * std::max(top - p.groundLevel, 1.0f);
		// NOTE: frand() is called in a fixed order below (x then z) — the two
		// horizontal offsets are NOT interchangeable, swapping them reshuffles
		// the whole pool.
		const float px   = p.cameraPos.x + (frand() * 2.0f - 1.0f) * p.boxHalf;
		const float pz   = p.cameraPos.z + (frand() * 2.0f - 1.0f) * p.boxHalf;
		// Snow is blown around far less than rain by the same wind vector.
		const float lateral = p.isSnow ? 0.3f : 1.2f;
		const float vx   = p.windVec.x * lateral;
		const float vy   = -p.fallSpeed;
		const float vz   = p.windVec.z * lateral;
		// Life is set so the drop expires exactly as it reaches the ground plane.
		const float life = (y - p.groundLevel) / std::max(p.fallSpeed, 0.01f);

		float* d = &out[static_cast<size_t>(i) * kWeatherParticleFloats];
		d[0] = px; d[1] = y; d[2] = pz;
		if (layout == WeatherParticleLayout::PosVelLifeSeed)
		{
			d[3] = vx; d[4] = vy; d[5] = vz;
			d[6] = life;
		}
		else
		{
			d[3] = life;
			d[4] = vx; d[5] = vy; d[6] = vz;
		}
		d[7] = seed;
	}
}

} // namespace HE
