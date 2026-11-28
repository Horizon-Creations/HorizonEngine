#include "StampHeightmap.h"

#include <HorizonScene/TerrainHeightmap.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace HE::StampHeightmap
{
namespace
{
float ease(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
float mix(float a, float b, float t) { return a + (b - a) * t; }
float sq(float v) { return v * v; }

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
// How far along A -> B the point lies (0 at A, 1 at B, unclamped).
float along(float px, float py, float x0, float y0, float x1, float y1)
{
	const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
	return len2 > 0.0f ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.0f;
}

// What a generator or a filter made, laid over what was there: clamped, and only as far as the mask lets it.
void finish(Map& m, const std::vector<float>& before, const Map* mask)
{
	const bool masked = mask && mask->v.size() == m.v.size();
	for (size_t i = 0; i < m.v.size(); ++i)
	{
		const float made = std::clamp(m.v[i], 0.0f, 1.0f);
		m.v[i] = masked ? mix(before[i], made, std::clamp(mask->v[i], 0.0f, 1.0f)) : made;
	}
}

// ── Noise ──
uint32_t hash(int x, int y, uint32_t seed)
{
	uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}
// Gradient noise, about -1..1, one feature per unit.
float perlin(float x, float y, uint32_t seed)
{
	const float flx = std::floor(x), fly = std::floor(y);
	const int ix = static_cast<int>(flx), iy = static_cast<int>(fly);
	const float fx = x - flx, fy = y - fly;
	const auto corner = [&](int cx, int cy) {
		const float a = static_cast<float>(hash(ix + cx, iy + cy, seed) & 0xffffu) * (6.2831853f / 65536.0f);
		return std::cos(a) * (fx - static_cast<float>(cx)) + std::sin(a) * (fy - static_cast<float>(cy));
	};
	const float u = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f), v = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
	return 1.41421356f * mix(mix(corner(0, 0), corner(1, 0), u), mix(corner(0, 1), corner(1, 1), u), v);
}
// Octaves of it at twice the frequency each, about -1..1.
float fbm(float x, float y, int octaves, float gain, uint32_t seed)
{
	float sum = 0.0f, amp = 1.0f, norm = 0.0f;
	for (int o = 0; o < octaves; ++o)
	{
		sum += amp * perlin(x, y, seed + static_cast<uint32_t>(o) * 101u);
		norm += amp;
		amp *= gain;
		x *= 2.0f; y *= 2.0f;
	}
	return sum / norm;
}
// Ridged multifractal, 0..1: sharp crests where the noise crosses zero, each octave weighted by the one above it, so
// detail gathers on the ridges and the valleys stay smooth. The look of mountains.
float ridged(float x, float y, int octaves, float gain, uint32_t seed)
{
	float sum = 0.0f, amp = 1.0f, norm = 0.0f, weight = 1.0f;
	for (int o = 0; o < octaves; ++o)
	{
		const float r = sq(1.0f - std::abs(perlin(x, y, seed + static_cast<uint32_t>(o) * 131u))) * weight;
		weight = std::clamp(r * 2.0f, 0.0f, 1.0f);
		sum += amp * r;
		norm += amp;
		amp *= gain;
		x *= 2.0f; y *= 2.0f;
	}
	return sum / norm;
}

float sampleBilinear(const Map& m, float x, float y)
{
	x = std::clamp(x, 0.0f, static_cast<float>(m.w - 1));
	y = std::clamp(y, 0.0f, static_cast<float>(m.h - 1));
	const int ix = std::min(static_cast<int>(x), std::max(0, m.w - 2)), iy = std::min(static_cast<int>(y), std::max(0, m.h - 2));
	const int jx = std::min(ix + 1, m.w - 1), jy = std::min(iy + 1, m.h - 1);
	const float fx = x - static_cast<float>(ix), fy = y - static_cast<float>(iy);
	const auto at = [&](int px, int py) { return m.v[static_cast<size_t>(py) * m.w + px]; };
	return mix(mix(at(ix, iy), at(jx, iy), fx), mix(at(ix, jy), at(jx, jy), fx), fy);
}

// Three box passes each way: close to a Gaussian of about that radius.
void blurField(std::vector<float>& v, int w, int h, float radius)
{
	const int r = std::max(1, static_cast<int>(std::lround(radius * 0.5f)));
	std::vector<float> tmp(v.size());
	const auto pass = [&](int count, int stride, int lines, int lineStride) {
		for (int l = 0; l < lines; ++l)
		{
			const size_t base = static_cast<size_t>(l) * lineStride;
			const auto at = [&](int i) { return v[base + static_cast<size_t>(std::clamp(i, 0, count - 1)) * stride]; };
			float sum = 0.0f;
			for (int i = -r; i <= r; ++i) sum += at(i);
			for (int i = 0; i < count; ++i)
			{
				tmp[base + static_cast<size_t>(i) * stride] = sum / static_cast<float>(2 * r + 1);
				sum += at(i + r + 1) - at(i - r);
			}
		}
		v.swap(tmp);
	};
	for (int n = 0; n < 3; ++n)
	{
		pass(w, 1, h, w);   // along the rows
		pass(h, w, w, 1);   // along the columns
	}
}

std::string s_editFile;
bool        s_editRequested = false;
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

void flatten(Map& m, float x, float y, float radius, float height, float strength)
{
	eachIn(m, x, y, x, y, radius, [&](size_t i, float px, float py) {
		m.v[i] = mix(m.v[i], std::clamp(height, 0.0f, 1.0f), std::clamp(strength, 0.0f, 1.0f) * falloff(std::hypot(px - x, py - y), radius));
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

void generate(Map& m, const GenParams& p, const Map* mask)
{
	const std::vector<float> before = m.v;
	const float scale = std::clamp(p.scale, 0.02f, 4.0f), f = 1.0f / scale;
	const int oct = std::clamp(p.detail, 1, 10);
	const float gain = std::clamp(p.roughness, 0.1f, 0.9f);
	for (int y = 0; y < m.h; ++y)
		for (int x = 0; x < m.w; ++x)
		{
			float u = m.w > 1 ? static_cast<float>(x) / static_cast<float>(m.w - 1) : 0.5f;
			float v = m.h > 1 ? static_cast<float>(y) / static_cast<float>(m.h - 1) : 0.5f;
			// Domain warp: the form is read at a place pushed about by slow noise.
			const float wx = fbm(u * f * 0.5f + 31.7f, v * f * 0.5f + 11.3f, 3, 0.5f, p.seed ^ 0x9e37u);
			const float wy = fbm(u * f * 0.5f - 17.1f, v * f * 0.5f + 47.9f, 3, 0.5f, p.seed ^ 0x51edu);
			u += p.warp * 0.5f * scale * wx;
			v += p.warp * 0.5f * scale * wy;
			const float r = std::hypot(u - 0.5f, v - 0.5f) * 2.0f;   // 0 in the middle, 1 at the edge's centre
			const float n = fbm(u * f, v * f, oct, gain, p.seed);
			const float rg = ridged(u * f, v * f, oct, gain, p.seed);
			float h = 0.0f;
			switch (p.shape)
			{
			case Shape::Hills:         h = 0.5f + 0.5f * n; break;
			case Shape::Mountain:      h = ease(1.0f - r) * (0.45f + 0.55f * rg); break;
			case Shape::MountainRange: h = std::pow(rg, 1.5f); break;
			case Shape::Plateau:       h = ease((0.72f - r * (1.0f + 0.25f * n)) / 0.18f) * (0.92f + 0.08f * n); break;
			case Shape::Crater:
			{
				const float rr = r / 0.62f * (1.0f + 0.08f * n);   // 1 on the rim
				h = (rr < 1.0f ? mix(0.12f, 1.0f, rr * rr * rr) : std::exp(-sq((rr - 1.0f) / 0.28f))) + 0.04f * n;
				break;
			}
			case Shape::Volcano:
			{
				float c = std::pow(std::clamp(1.0f - r / 0.9f, 0.0f, 1.0f), 1.3f) * (0.8f + 0.2f * rg);
				if (c > 0.72f) c = 0.72f - (c - 0.72f) * 1.6f;   // the top is cut and sinks: the crater
				h = c / 0.72f;
				break;
			}
			case Shape::Dunes:
			{
				const float phase = u * f * 2.0f + 0.6f * fbm(u * f * 0.3f, v * f * 0.3f, 2, 0.5f, p.seed + 977u);
				const float t = phase - std::floor(phase);
				h = (t < 0.7f ? ease(t / 0.7f) : ease((1.0f - t) / 0.3f)) * (0.65f + 0.35f * (0.5f + 0.5f * n));   // long windward, short lee
				break;
			}
			case Shape::Island:        h = (0.55f + 0.45f * n) * 1.25f - r * r * 1.15f; break;
			default: break;
			}
			h = std::clamp(h, 0.0f, 1.0f) * p.height;
			float& out = m.v[static_cast<size_t>(y) * m.w + x];
			switch (p.blend)
			{
			case Blend::Add:      out += h; break;
			case Blend::Subtract: out -= h; break;
			case Blend::Max:      out = std::max(out, h); break;
			case Blend::Min:      out = std::min(out, h); break;
			default:              out = h; break;
			}
		}
	finish(m, before, mask);
}

void refine(Map& m, const RefineParams& p, const Map* mask)
{
	const std::vector<float> before = m.v;
	blurField(m.v, m.w, m.h, std::max(1.0f, p.smoothing * static_cast<float>(std::max(m.w, m.h))));
	const Map base = m;
	for (int y = 0; y < m.h; ++y)
		for (int x = 0; x < m.w; ++x)
		{
			const float u = m.w > 1 ? static_cast<float>(x) / static_cast<float>(m.w - 1) : 0.5f;
			const float v = m.h > 1 ? static_cast<float>(y) / static_cast<float>(m.h - 1) : 0.5f;
			const float wx = fbm(u * 3.0f + 5.3f, v * 3.0f + 1.7f, 3, 0.5f, p.seed ^ 0x7f4au);
			const float wy = fbm(u * 3.0f - 9.1f, v * 3.0f + 8.2f, 3, 0.5f, p.seed ^ 0x1b56u);
			const float b = sampleBilinear(base, (u + p.warp * 0.08f * wx) * static_cast<float>(m.w - 1), (v + p.warp * 0.08f * wy) * static_cast<float>(m.h - 1));
			// Relief grows with the height: plains stay calm, heights get crags. Ground the sketch left at zero stays zero.
			const float amp = p.detail * (0.08f + 0.92f * b) * ease(b * 20.0f);
			const float relief = (ridged(u * 6.0f, v * 6.0f, 6, 0.5f, p.seed) - 0.5f) * 0.6f + fbm(u * 12.0f, v * 12.0f, 5, 0.5f, p.seed + 7u) * 0.15f;
			m.v[static_cast<size_t>(y) * m.w + x] = std::clamp(b + amp * relief, 0.0f, 1.0f);
		}
	if (p.erosion > 0.0f) erode(m, p.erosion, 2 + static_cast<int>(p.erosion * 6.0f), p.seed);
	finish(m, before, mask);
}

void erode(Map& m, float amount, int thermalPasses, uint32_t seed, const Map* mask)
{
	const int w = m.w, h = m.h;
	if (w < 4 || h < 4) return;
	const std::vector<float> before = m.v;
	// Heights in pixels, so a slope is a slope whatever the picture's size: a full-height peak is 0.3 edges tall.
	const float unit = 0.3f * static_cast<float>(std::max(w, h));
	std::vector<float> g(m.v.size());
	for (size_t i = 0; i < g.size(); ++i) g[i] = m.v[i] * unit;

	// Water, drop by drop (after Hans Beyer's "Implementation of a method for hydraulic erosion", 2015): a drop runs downhill,
	// takes ground while it can carry more and lays it down where it slows. It takes with a small round brush, or every
	// channel would be one pixel wide.
	struct Tap { int dx, dy; float weight; };
	std::vector<Tap> brush;
	float brushSum = 0.0f;
	for (int dy = -2; dy <= 2; ++dy)
		for (int dx = -2; dx <= 2; ++dx)
			if (const float d = std::hypot(static_cast<float>(dx), static_cast<float>(dy)); d <= 2.0f)
			{
				brush.push_back({ dx, dy, 1.0f - d / 3.0f });
				brushSum += brush.back().weight;
			}
	for (Tap& t : brush) t.weight /= brushSum;

	uint32_t rng = seed * 747796405u + 2891336453u;
	const auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return static_cast<float>(rng & 0xffffffu) / 16777216.0f; };
	const auto heightAt = [&](float px, float py) {
		const int ix = static_cast<int>(px), iy = static_cast<int>(py);
		const float fx = px - static_cast<float>(ix), fy = py - static_cast<float>(iy);
		const size_t i = static_cast<size_t>(iy) * w + ix;
		return mix(mix(g[i], g[i + 1], fx), mix(g[i + w], g[i + w + 1], fx), fy);
	};
	// shortcut: runs on the calling thread and is capped at 300 000 drops, so a 1024 picture is eroded more thinly than a 256 one; move it to the job pool when that shows
	const int drops = std::min(300000, static_cast<int>(std::clamp(amount, 0.0f, 1.0f) * 0.5f * static_cast<float>(w) * static_cast<float>(h)));
	const int life = std::clamp(std::max(w, h) / 4, 30, 128);
	for (int d = 0; d < drops; ++d)
	{
		float px = rnd() * (static_cast<float>(w) - 1.001f), py = rnd() * (static_cast<float>(h) - 1.001f);
		float dx = 0.0f, dy = 0.0f, speed = 1.0f, water = 1.0f, carried = 0.0f;
		for (int step = 0; step < life; ++step)
		{
			const int ix = static_cast<int>(px), iy = static_cast<int>(py);
			const float fx = px - static_cast<float>(ix), fy = py - static_cast<float>(iy);
			const size_t i = static_cast<size_t>(iy) * w + ix;
			const float gx = mix(g[i + 1] - g[i], g[i + w + 1] - g[i + w], fy), gy = mix(g[i + w] - g[i], g[i + w + 1] - g[i + 1], fx);
			const float hOld = heightAt(px, py);
			dx = dx * 0.05f - gx * 0.95f;   // a little inertia, mostly downhill
			dy = dy * 0.05f - gy * 0.95f;
			const float len = std::hypot(dx, dy);
			if (len < 1e-6f) break;   // a pit or a plain: the drop stays
			dx /= len; dy /= len;
			const float nx = px + dx, ny = py + dy;
			if (nx < 0.0f || ny < 0.0f || nx >= static_cast<float>(w - 1) || ny >= static_cast<float>(h - 1)) break;
			const float dh = heightAt(nx, ny) - hOld;
			const float capacity = std::max(-dh, 0.01f) * speed * water * 4.0f;
			if (carried > capacity || dh > 0.0f)
			{
				// Uphill: fill the hollow behind. Otherwise drop a share of what is too much.
				const float lay = dh > 0.0f ? std::min(dh, carried) : (carried - capacity) * 0.3f;
				carried -= lay;
				g[i] += lay * (1.0f - fx) * (1.0f - fy);
				g[i + 1] += lay * fx * (1.0f - fy);
				g[i + w] += lay * (1.0f - fx) * fy;
				g[i + w + 1] += lay * fx * fy;
			}
			else
			{
				const float take = std::min((capacity - carried) * 0.3f, -dh);   // never deeper than the step down
				for (const Tap& t : brush)
				{
					const int cx = ix + t.dx, cy = iy + t.dy;
					if (cx < 0 || cy < 0 || cx >= w || cy >= h) continue;
					float& cell = g[static_cast<size_t>(cy) * w + cx];
					const float got = std::min(take * t.weight, cell);
					cell -= got;
					carried += got;
				}
			}
			speed = std::sqrt(std::max(0.0f, speed * speed - dh * 4.0f));
			water *= 0.99f;
			px = nx; py = ny;
		}
	}

	// What the drops laid down lies in single pixels: half a light blur settles it without closing the channels.
	if (drops > 0)
	{
		std::vector<float> soft = g;
		blurField(soft, w, h, 2.0f);
		for (size_t i = 0; i < g.size(); ++i) g[i] = mix(g[i], soft[i], 0.5f);
	}

	// Gravity: what is steeper than the angle of repose slides to the lowest neighbour.
	const float talus = 0.5f;
	std::vector<float> delta(g.size());
	for (int pass = 0; pass < thermalPasses; ++pass)
	{
		std::fill(delta.begin(), delta.end(), 0.0f);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const size_t c = static_cast<size_t>(y) * w + x;
				size_t low = c;
				if (x > 0 && g[c - 1] < g[low]) low = c - 1;
				if (x < w - 1 && g[c + 1] < g[low]) low = c + 1;
				if (y > 0 && g[c - w] < g[low]) low = c - w;
				if (y < h - 1 && g[c + w] < g[low]) low = c + w;
				const float diff = g[c] - g[low];
				if (diff <= talus) continue;
				delta[c] -= (diff - talus) * 0.25f;
				delta[low] += (diff - talus) * 0.25f;
			}
		for (size_t i = 0; i < g.size(); ++i) g[i] += delta[i];
	}

	for (size_t i = 0; i < g.size(); ++i) m.v[i] = g[i] / unit;
	finish(m, before, mask);
}

void blur(Map& m, float radius, const Map* mask)
{
	const std::vector<float> before = m.v;
	blurField(m.v, m.w, m.h, radius);
	finish(m, before, mask);
}

void terrace(Map& m, int steps, float sharpness, const Map* mask)
{
	const std::vector<float> before = m.v;
	const float n = static_cast<float>(std::max(1, steps));
	for (float& v : m.v)
	{
		const float t = v * n, level = std::floor(t);
		v = mix(v, (level + ease((t - level - 0.35f) / 0.3f)) / n, std::clamp(sharpness, 0.0f, 1.0f));   // flat treads, short risers
	}
	finish(m, before, mask);
}

void normalize(Map& m)
{
	if (m.v.empty()) return;
	const auto [lo, hi] = std::minmax_element(m.v.begin(), m.v.end());
	const float low = *lo, range = *hi - *lo;
	if (range <= 0.0f) return;
	for (float& v : m.v) v = (v - low) / range;
}

void invert(Map& m, const Map* mask)
{
	const std::vector<float> before = m.v;
	for (float& v : m.v) v = 1.0f - v;
	finish(m, before, mask);
}

void fadeEdges(Map& m, float width)
{
	if (width <= 0.0f) return;
	for (int y = 0; y < m.h; ++y)
		for (int x = 0; x < m.w; ++x)
		{
			const float dx = m.w > 1 ? 1.0f - std::abs(static_cast<float>(x) / static_cast<float>(m.w - 1) * 2.0f - 1.0f) : 1.0f;
			const float dy = m.h > 1 ? 1.0f - std::abs(static_cast<float>(y) / static_cast<float>(m.h - 1) * 2.0f - 1.0f) : 1.0f;
			m.v[static_cast<size_t>(y) * m.w + x] *= ease(dx / width) * ease(dy / width);
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

void requestEdit(const std::string& file) { s_editFile = file; s_editRequested = true; }
bool takeEditRequest(std::string& file)
{
	if (!s_editRequested) return false;
	s_editRequested = false;
	file = std::move(s_editFile);
	return true;
}
}
