#pragma once
#include <Math/Math.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// ─── CPU sky bake for the image-based ambient cubemap ────────────────────────
// A CPU port of the sky shaders' analytic skyColor(dir, sunDir). The scene
// shaders sample the baked cubemap once per lit pixel instead of evaluating
// skyColor twice, so this has to agree with kSkyFuncGLSL / kSkyFuncMSL — it is a
// mirror of them, not an approximation, EXCEPT for the sun terms: the bake keeps
// a sun disk on purpose so the image-based ambient carries the sun's energy.
//
// This was 88 byte-identical lines in OpenGLRenderer.cpp and MetalRenderer.mm
// (plus a duplicated face-direction switch). The OUTPUT IS OBSERVABLE — it is
// the ambient light every lit surface receives — and the two backends must agree
// pixel for pixel, so it is pinned by a hash in tests/test_culling.cpp. Changing
// any constant below changes the lighting on every platform at once; re-pin the
// test consciously.
//
// Header-only: pure functions with no state, and the test executable compiles a
// fixed list of shared HE_Rendering sources, so a new .cpp here would not reach
// it without a build-file change.
namespace HE
{

// Ray/sphere intersection in the atmosphere model's coordinates (planet centre at
// the origin). Returns the two hit distances; x > y means "no hit".
//
// "No hit" returns a NEGATIVE near distance — see the shader comment: the caller's
// sun-visibility test is `.x > 0` → shadowed, and a miss is precisely the case
// where the sun IS visible, so a positive sentinel collapsed the sky to black at
// sunset.
inline glm::vec2 AtmoRaySphereCPU(glm::vec3 ro, glm::vec3 rd, float R)
{
	float b = glm::dot(ro, rd);
	float c = glm::dot(ro, ro) - R * R;
	float d = b * b - c;
	if (d < 0.0f) return glm::vec2(-1.0e9f, -1.0e9f);
	d = std::sqrt(d);
	return glm::vec2(-b - d, -b + d);
}

// Mirror of the shader atmoScatter() — see kSkyFuncGLSL / kSkyFuncMSL for the
// model notes (Rayleigh + Mie + ozone, 12 view samples x 5 light samples).
inline glm::vec3 AtmoScatterCPU(glm::vec3 dir, glm::vec3 sunDir)
{
	const float Rg = 6360.0e3f, Ra = 6440.0e3f;
	const glm::vec3 bR(5.802e-6f, 13.558e-6f, 33.1e-6f);
	const float bM = 3.996e-6f;
	const glm::vec3 bO(0.650e-6f, 1.881e-6f, 0.085e-6f);
	const float HR = 8500.0f, HM = 1200.0f;
	glm::vec3 ro(0.0f, Rg + 200.0f, 0.0f);
	glm::vec2 tA = AtmoRaySphereCPU(ro, dir, Ra);
	if (tA.y <= 0.0f) return glm::vec3(0.0f);
	float t0 = std::max(tA.x, 0.0f), t1 = tA.y;
	glm::vec2 tG = AtmoRaySphereCPU(ro, dir, Rg);
	if (tG.x > 0.0f) t1 = std::min(t1, tG.x);
	float ds = (t1 - t0) / 12.0f;
	float mu = glm::dot(dir, sunDir);
	float phR = 0.05968310f * (1.0f + mu * mu);
	const float g = 0.76f, g2 = g * g;
	float phM = 0.11936620f * ((1.0f - g2) * (1.0f + mu * mu)) /
	            ((2.0f + g2) * std::pow(1.0f + g2 - 2.0f * g * mu, 1.5f));
	glm::vec3 sumR(0.0f), sumM(0.0f);
	float odR = 0.0f, odM = 0.0f, odO = 0.0f;
	for (int i = 0; i < 12; ++i)
	{
		glm::vec3 p = ro + dir * (t0 + (float(i) + 0.5f) * ds);
		float hgt = glm::length(p) - Rg;
		float dR  = std::exp(-hgt / HR) * ds;
		float dM  = std::exp(-hgt / HM) * ds;
		float dO  = std::max(0.0f, 1.0f - std::abs(hgt - 25.0e3f) / 15.0e3f) * ds;
		odR += dR; odM += dM; odO += dO;
		if (AtmoRaySphereCPU(p, sunDir, Rg).x > 0.0f) continue;
		float sl = AtmoRaySphereCPU(p, sunDir, Ra).y * 0.2f;
		float sR = 0.0f, sM = 0.0f, sO = 0.0f;
		for (int j = 0; j < 5; ++j)
		{
			glm::vec3 q = p + sunDir * ((float(j) + 0.5f) * sl);
			float hq = glm::length(q) - Rg;
			sR += std::exp(-hq / HR) * sl;
			sM += std::exp(-hq / HM) * sl;
			sO += std::max(0.0f, 1.0f - std::abs(hq - 25.0e3f) / 15.0e3f) * sl;
		}
		glm::vec3 tau = bR * (odR + sR) + (bM * 1.11f) * (odM + sM) + bO * (odO + sO);
		glm::vec3 tr  = glm::exp(-tau);
		sumR += tr * dR;
		sumM += tr * dM;
	}
	glm::vec3 L = (sumR * bR * phR + sumM * bM * phM) * 20.0f;
	// Fake multiple-scatter in-fill — mirrors the shader atmoScatter().
	glm::vec3 Tcam = glm::exp(-(bR * odR + (bM * 1.11f) * odM + bO * odO));
	L += (glm::vec3(1.0f) - Tcam) * glm::vec3(0.30f, 0.42f, 0.60f) * (0.35f * glm::smoothstep(0.0f, 0.35f, sunDir.y));
	return L;
}

// Radiance arriving from `dir` for the given sun direction. Both are normalised
// internally, so callers may pass unnormalised face directions.
inline glm::vec3 SkyColorCPU(glm::vec3 dir, glm::vec3 sunDir)
{
	dir = glm::normalize(dir); sunDir = glm::normalize(sunDir);
	float sunY = glm::clamp(sunDir.y, -0.3f, 1.0f);
	float day  = glm::smoothstep(-0.10f, 0.10f, sunY);
	float dusk = glm::smoothstep(-0.14f, 0.04f, sunY) * (1.0f - glm::smoothstep(0.04f, 0.26f, sunY));
	// Raw (unclamped) elevation for the two NIGHT ramps — see the shader comment:
	// sunY saturates at -0.3, so twilight would never reach zero at midnight.
	float sunYd = sunDir.y;
	float toNight = 1.0f - glm::smoothstep(-0.34f, -0.14f, sunYd);
	// Physically-based base sky (mirrors the shader), plus the deep-night floor.
	glm::vec3 sky = AtmoScatterCPU(glm::normalize(glm::vec3(dir.x, std::max(dir.y, 0.004f), dir.z)), sunDir); // horizon clamp (see shader)
	// Twilight wedge — mirrors the shader; see kSkyFuncMSL for the full rationale.
	float twi = glm::smoothstep(0.10f, -0.01f, sunY) * glm::smoothstep(-0.36f, -0.12f, sunYd);
	if (twi > 0.0f)
	{
		glm::vec2 sunAz = glm::normalize(glm::vec2(sunDir.x, sunDir.z) + glm::vec2(1e-5f));
		glm::vec2 dxz(dir.x, dir.z);
		float hlen   = glm::length(dxz);
		float toward = (hlen > 1e-4f)
			? glm::clamp(glm::dot(dxz / hlen, sunAz) * 0.5f + 0.5f, 0.0f, 1.0f) : 0.5f;
		toward = glm::mix(0.5f, toward, glm::smoothstep(0.0f, 0.06f, hlen));
		float el     = dir.y;                                       // SIGNED — see `above`
		float band   = std::exp(-std::max(el, 0.0f) * 5.2f);
		float climb  = glm::clamp(std::max(el, 0.0f) * 3.4f, 0.0f, 1.0f);
		float above  = glm::smoothstep(-0.22f, -0.01f, el);
		glm::vec3 warm(1.00f, 0.45f, 0.17f), mid(0.62f, 0.34f, 0.52f), cool(0.20f, 0.30f, 0.62f);
		glm::vec3 col = glm::mix(glm::mix(warm, mid, glm::smoothstep(0.0f, 0.50f, climb)),
		                         cool, glm::smoothstep(0.35f, 1.0f, climb));
		col = glm::mix(cool, col, toward * toward);
		sky += col * (twi * above * (0.065f + 0.34f * band * toward * toward));
	}
	float h = glm::clamp(dir.y, 0.0f, 1.0f);
	sky += glm::mix(glm::vec3(0.006f,0.009f,0.024f), glm::vec3(0.003f,0.005f,0.015f), h) * toNight;
	glm::vec3 ground = glm::mix(sky * 0.32f, glm::vec3(0.24f,0.23f,0.21f), day);
	sky = glm::mix(sky, ground, glm::smoothstep(0.0f, -0.20f, dir.y));
	// CPU mirror deliberately keeps a sun DISK (IBL ambient carries sun energy).
	glm::vec3 sunTint = glm::mix(glm::vec3(1.0f,0.42f,0.20f), glm::vec3(1.0f,0.96f,0.88f), glm::smoothstep(0.0f,0.25f,sunY));
	float s = std::max(glm::dot(dir, sunDir), 0.0f);
	float sunVis = std::max(day, dusk);
	sky += sunTint * (std::pow(s,1800.0f) * 14.0f * day);
	sky += sunTint * (std::pow(s,180.0f)  * 2.2f * sunVis);
	sky += sunTint * (std::pow(s,22.0f)   * 0.7f * sunVis);
	float night = 1.0f - day;
	glm::vec3 moonDir = glm::normalize(glm::vec3(-sunDir.x, -sunDir.y, sunDir.z));
	float mdot = std::max(glm::dot(dir, moonDir), 0.0f);
	sky += glm::vec3(0.80f,0.86f,1.00f) * (std::pow(mdot,60.0f) * 0.05f * night);
	sky += glm::vec3(0.015f,0.018f,0.030f) * night;
	return sky;
}

// Cube faces in slice order +X,-X,+Y,-Y,+Z,-Z, with (u,v) in [-1,1]. GL and
// Metal cube maps use the SAME face/texel convention, so there is deliberately
// no per-backend axis flip here — verified lossless against a per-pixel
// skyColor evaluation (max 1/255).
inline glm::vec3 SkyEnvFaceDirection(int face, float u, float v)
{
	switch (face) {
		case 0: return glm::vec3( 1.0f, -v, -u); // +X
		case 1: return glm::vec3(-1.0f, -v,  u); // -X
		case 2: return glm::vec3( u,  1.0f,  v); // +Y
		case 3: return glm::vec3( u, -1.0f, -v); // -Y
		case 4: return glm::vec3( u, -v,  1.0f); // +Z
		default:return glm::vec3(-u, -v, -1.0f); // -Z
	}
}

// One texel ROW of one face as tightly packed RGBA32F (faceN * 4 floats written
// to `outRow`). Row granularity is what lets a caller parallelise the bake
// without owning any of the maths: each row is independent and SkyColorCPU is
// pure, so a row-per-task fan-out is data-race-free.
inline void BuildSkyEnvFaceRow(int faceN, int face, int row,
                               const glm::vec3& sunDir, float* outRow)
{
	const float v = (row + 0.5f) / faceN * 2.0f - 1.0f;
	for (int s = 0; s < faceN; ++s)
	{
		const float u = (s + 0.5f) / faceN * 2.0f - 1.0f;
		const glm::vec3 c = SkyColorCPU(glm::normalize(SkyEnvFaceDirection(face, u, v)), sunDir);
		float* px = outRow + static_cast<size_t>(s) * 4;
		px[0] = c.r; px[1] = c.g; px[2] = c.b; px[3] = 1.0f;
	}
}

// One whole face as tightly packed RGBA32F, faceN * faceN texels. Serial — a
// caller that wants the six faces in parallel should fan out over
// BuildSkyEnvFaceRow instead.
inline std::vector<float> BuildSkyEnvFace(int faceN, int face, const glm::vec3& sunDir)
{
	std::vector<float> px(static_cast<size_t>(faceN) * faceN * 4);
	for (int t = 0; t < faceN; ++t)
		BuildSkyEnvFaceRow(faceN, face, t, sunDir, &px[static_cast<size_t>(t) * faceN * 4]);
	return px;
}

} // namespace HE
