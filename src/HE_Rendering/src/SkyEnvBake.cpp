#include "HorizonRendering/SkyEnvBake.h"
#include <Diagnostics/Profiler.h>
#include <JobSystem/JobSystem.h>
#include <glm/gtc/packing.hpp>
#include <chrono>

namespace HE
{

namespace
{
double clockSeconds()
{
	using clk = std::chrono::steady_clock;
	return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}
} // namespace

float SkyEnvSunTolerance(const glm::vec3& bakedSun, const glm::vec3& sunDir)
{
	const float a = glm::normalize(bakedSun).y;
	const float b = glm::normalize(sunDir).y;
	if (a > kSkyEnvTwilightHigh && b > kSkyEnvTwilightHigh) return kSkyEnvSunToleranceDay;
	if (a < kSkyEnvTwilightLow  && b < kSkyEnvTwilightLow)  return kSkyEnvSunToleranceNight;
	return kSkyEnvSunTolerance;
}

bool SkyEnvBaker::due(const glm::vec3& sunDir) const
{
	if (!m_baked) return true;
	return glm::distance(sunDir, m_sunDir) >= SkyEnvSunTolerance(m_sunDir, sunDir);
}

// Rows are independent and SkyColorCPU is pure, so a (face, row) fan-out is
// data-race-free. Grain 1: a row is tens of µs of work, far above the default
// grain's assumption of a cheap per-index body.
const float* SkyEnvBaker::bakeFloat(const glm::vec3& sunDir)
{
	HE_PROFILE_SCOPE_N("SkyEnvBaker::bake");
	const double start = clockSeconds();
	const int N = m_faceN;
	m_float.resize(channels());
	float* const px = m_float.data();
	parallel_for(static_cast<size_t>(6) * N, [&](size_t idx)
	{
		const int f = static_cast<int>(idx / N);
		const int t = static_cast<int>(idx % N);
		BuildSkyEnvFaceRow(N, f, t, sunDir, px + ((static_cast<size_t>(f) * N + t) * N) * 4);
	}, "SkyEnvBake", 1);
	finish(sunDir, start);
	return px;
}

const uint16_t* SkyEnvBaker::bakeHalf(const glm::vec3& sunDir, uint16_t* dst)
{
	HE_PROFILE_SCOPE_N("SkyEnvBaker::bake");
	const double start = clockSeconds();
	const int N = m_faceN;
	if (!dst)
	{
		m_half.resize(channels());
		dst = m_half.data();
	}
	uint16_t* const out = dst;
	parallel_for(static_cast<size_t>(6) * N, [&](size_t idx)
	{
		const int f = static_cast<int>(idx / N);
		const int t = static_cast<int>(idx % N);
		// The row in floats on the task's stack, packed right here — the old
		// drivers converted all 393k channels serially after the fan-out.
		float row[kSkyEnvFaceSize * 4];
		std::vector<float> wide;
		float* rowPx = row;
		if (N > kSkyEnvFaceSize) { wide.resize(static_cast<size_t>(N) * 4); rowPx = wide.data(); }
		BuildSkyEnvFaceRow(N, f, t, sunDir, rowPx);
		uint16_t* const o = out + ((static_cast<size_t>(f) * N + t) * N) * 4;
		for (int i = 0; i < N * 4; ++i)
			o[i] = glm::packHalf1x16(rowPx[i]);
	}, "SkyEnvBake", 1);
	finish(sunDir, start);
	return out;
}

void SkyEnvBaker::finish(const glm::vec3& sunDir, double startSeconds)
{
	m_sunDir     = sunDir;
	m_baked      = true;
	++m_bakeCount;
	m_lastBakeMs = (clockSeconds() - startSeconds) * 1000.0;
}

} // namespace HE
