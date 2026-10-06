#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ─── HE_GI_SHADOW_BENCH: GPU time of the GI sun-ray dispatch (Thema 142) ────
// The Windows-side twin of Metal's HE_GI_PROTO_BENCH. With HE_GI_SHADOW_BENCH=1
// the D3D11 and GL backends bracket the shadow-ray compute dispatch with a timer
// query, read it back the same frame (a stall, measurement only) and log
// p10/p50/p90 per window of frames. Whole-frame timestamps cannot answer
// "what does one more sun ray cost": in a headless dump they mostly measure the
// CPU building the frame. Off (the default) = not one extra API call.
// docs/gi-shadow-restflackern-1spp-2026-10-03.md §8.4.
namespace HE
{

struct GIShadowBench
{
	static bool enabled()
	{
		static const bool on = [] {
			const char* v = std::getenv("HE_GI_SHADOW_BENCH");
			return v && *v && std::atoi(v) != 0;
		}();
		return on;
	}

	static constexpr int kWarmup = 40;    // pipeline creation, clock ramp, history fill
	static constexpr int kWindow = 160;   // a 240-frame dump logs exactly one window

	// One sample per frame. Returns a finished log line once per window, else "".
	std::string add(double ms, int rays, int instances, int width, int height)
	{
		if (++m_seen <= kWarmup || ms <= 0.0) return {};
		m_ms.push_back(ms);
		if (static_cast<int>(m_ms.size()) < kWindow) return {};
		std::sort(m_ms.begin(), m_ms.end());
		auto pct = [&](double p) { return m_ms[static_cast<size_t>(p * double(m_ms.size() - 1) + 0.5)]; };
		char line[220];
		std::snprintf(line, sizeof line,
			"GI shadow bench: sun-ray dispatch GPU ms p10 %.4f p50 %.4f p90 %.4f "
			"(rays %d, instances %d, mask %dx%d, %zu frames)",
			pct(0.1), pct(0.5), pct(0.9), rays, instances, width, height, m_ms.size());
		m_ms.clear();
		return line;
	}

private:
	std::vector<double> m_ms;
	int m_seen = 0;
};

} // namespace HE
