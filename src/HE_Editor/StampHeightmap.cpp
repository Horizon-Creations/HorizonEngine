#include "StampHeightmap.h"

#include <HorizonScene/TerrainHeightmap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace HE::StampHeightmap
{
namespace
{
float ease(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
float mix(float a, float b, float t) { return a + (b - a) * t; }

// Calls f(index, px, py) for the pixels of the box [x0-r, x1+r] x [y0-r, y1+r] that lie in the picture.
template <class F>
void eachIn(Map& m, float x0, float y0, float x1, float y1, float r, F f)
{
	const int ax = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - r)));
	const int ay = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - r)));
	const int bx = std::min(m.w - 1, static_cast<int>(std::ceil(std::max(x0, x1) + r)));
	const int by = std::min(m.h - 1, static_cast<int>(std::ceil(std::max(y0, y1) + r)));
	for (int y = ay; y <= by; ++y)
		for (int x = ax; x <= bx; ++x)
			f(static_cast<size_t>(y) * m.w + x, static_cast<float>(x), static_cast<float>(y));
}
// 1 at the centre, 0 from `radius` on.
float falloff(float dist, float radius) { return radius > 0.0f ? ease(1.0f - dist / radius) : 0.0f; }
// How far along A -> B the point lies (0..1), unclamped when `clampT` is off.
float along(float px, float py, float x0, float y0, float x1, float y1)
{
	const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
	return len2 > 0.0f ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.0f;
}
} // namespace

void fill(Map& m, int w, int h, float height)
{
	m.w = std::max(1, w);
	m.h = std::max(1, h);
	m.v.assign(static_cast<size_t>(m.w) * m.h, std::clamp(height, 0.0f, 1.0f));
}

void dab(Map& m, float x, float y, float radius, float amount)
{
	eachIn(m, x, y, x, y, radius, [&](size_t i, float px, float py) {
		m.v[i] = std::clamp(m.v[i] + amount * falloff(std::hypot(px - x, py - y), radius), 0.0f, 1.0f);
	});
}

void smooth(Map& m, float x, float y, float radius, float strength)
{
	const std::vector<float> src = m.v;   // the means are of the picture before the dab
	eachIn(m, x, y, x, y, radius, [&](size_t i, float px, float py) {
		const int cx = static_cast<int>(px), cy = static_cast<int>(py);
		float sum = 0.0f;
		int n = 0;
		for (int oy = -1; oy <= 1; ++oy)
			for (int ox = -1; ox <= 1; ++ox)
			{
				const int sx = cx + ox, sy = cy + oy;
				if (sx < 0 || sy < 0 || sx >= m.w || sy >= m.h) continue;
				sum += src[static_cast<size_t>(sy) * m.w + sx];
				++n;
			}
		m.v[i] = mix(m.v[i], sum / static_cast<float>(n), std::clamp(strength, 0.0f, 1.0f) * falloff(std::hypot(px - x, py - y), radius));
	});
}

void line(Map& m, float x0, float y0, float x1, float y1, float radius, float h0, float h1, float strength)
{
	eachIn(m, x0, y0, x1, y1, radius, [&](size_t i, float px, float py) {
		const float t = std::clamp(along(px, py, x0, y0, x1, y1), 0.0f, 1.0f);
		const float d = std::hypot(px - mix(x0, x1, t), py - mix(y0, y1, t));
		m.v[i] = std::clamp(mix(m.v[i], mix(h0, h1, t), std::clamp(strength, 0.0f, 1.0f) * falloff(d, radius)), 0.0f, 1.0f);
	});
}

void gradient(Map& m, float x0, float y0, float x1, float y1, float h0, float h1, float strength)
{
	for (int y = 0; y < m.h; ++y)
		for (int x = 0; x < m.w; ++x)
		{
			float& v = m.v[static_cast<size_t>(y) * m.w + x];
			const float t = ease(along(static_cast<float>(x), static_cast<float>(y), x0, y0, x1, y1));
			v = std::clamp(mix(v, mix(h0, h1, t), std::clamp(strength, 0.0f, 1.0f)), 0.0f, 1.0f);
		}
}

bool writePgm16(const Map& m, const std::string& path, std::string& error)
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) { error = "cannot write " + path; return false; }
	f << "P5\n" << m.w << " " << m.h << "\n65535\n";
	std::vector<char> bytes(m.v.size() * 2);
	for (size_t i = 0; i < m.v.size(); ++i)
	{
		const uint16_t u = static_cast<uint16_t>(std::lround(std::clamp(m.v[i], 0.0f, 1.0f) * 65535.0f));
		bytes[i * 2] = static_cast<char>(u >> 8);   // PGM is big-endian
		bytes[i * 2 + 1] = static_cast<char>(u & 0xff);
	}
	f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	if (!f) { error = "cannot write " + path; return false; }
	return true;
}

bool read(Map& m, const std::string& path, std::string& error)
{
	std::vector<uint16_t> grey;
	uint32_t w = 0, h = 0;
	if (!TerrainHeightmap::readGrey16(path, grey, w, h, error) || w < 1 || h < 1) return false;
	m.w = static_cast<int>(w);
	m.h = static_cast<int>(h);
	m.v.resize(grey.size());
	for (size_t i = 0; i < grey.size(); ++i) m.v[i] = static_cast<float>(grey[i]) / 65535.0f;
	return true;
}
}
