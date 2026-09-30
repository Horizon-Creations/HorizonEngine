#include "HorizonRendering/SkyNoise3D.h"
#include <Math/Math.h>
#include <algorithm>
#include <cmath>
#include <thread>

namespace HE
{

std::vector<uint16_t> BuildSkyNoise3D(int n)
{
	auto hash = [](glm::vec3 p) {
		p = glm::fract(p * 0.1031f);
		p += glm::dot(p, glm::vec3(p.z, p.y, p.x) + 31.32f);
		return glm::fract((p.x + p.y) * p.z);
	};
	// Decorrelated per-cell jitter for the Worley feature points (sin-free so it is
	// bit-deterministic across compilers — every backend bakes CPU-side).
	auto hash3 = [](glm::vec3 c) {
		glm::vec3 p = glm::fract(c * glm::vec3(0.1031f, 0.1030f, 0.0973f));
		p += glm::dot(p, glm::vec3(p.y, p.z, p.x) + 33.33f);
		return glm::fract(glm::vec3((p.x + p.y) * p.z, (p.x + p.z) * p.y, (p.y + p.z) * p.x));
	};
	const int kWorleyGrid = 48;   // feature cells per axis across the tile
	// Jitter of every Worley cell, hashed once: 48³ hash3 calls instead of 27 per
	// voxel (~453 M at n = 256, which took ~10.7 s at renderer start, perf audit B7).
	// The wrapped cell coordinate is an integer-valued float, so hash3 sees exactly
	// the inputs it saw inline and the bytes stay pinned.
	std::vector<glm::vec3> jitter(static_cast<size_t>(kWorleyGrid) * kWorleyGrid * kWorleyGrid);
	for (int cz = 0; cz < kWorleyGrid; ++cz)
		for (int cy = 0; cy < kWorleyGrid; ++cy)
			for (int cx = 0; cx < kWorleyGrid; ++cx)
				jitter[(static_cast<size_t>(cz) * kWorleyGrid + cy) * kWorleyGrid + cx] = hash3(
					glm::vec3(static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(cz)));
	auto wrapCell = [&](int c) { return ((c % kWorleyGrid) + kWorleyGrid) % kWorleyGrid; }; // seamless tile
	auto worley = [&](glm::vec3 uv) {
		glm::vec3 pc = uv * static_cast<float>(kWorleyGrid);
		glm::vec3 id = glm::floor(pc);
		glm::vec3 fp = pc - id;
		const int ix = static_cast<int>(id.x), iy = static_cast<int>(id.y), iz = static_cast<int>(id.z);
		const int wx[3] = { wrapCell(ix - 1), wrapCell(ix), wrapCell(ix + 1) };
		const int wy[3] = { wrapCell(iy - 1), wrapCell(iy), wrapCell(iy + 1) };
		const int wz[3] = { wrapCell(iz - 1), wrapCell(iz), wrapCell(iz + 1) };
		float f1 = 1e9f;
		for (int k = -1; k <= 1; ++k)
			for (int j = -1; j <= 1; ++j)
				for (int i = -1; i <= 1; ++i)
				{
					glm::vec3 off(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k));
					const glm::vec3& h = jitter[(static_cast<size_t>(wz[k + 1]) * kWorleyGrid
					                             + wy[j + 1]) * kWorleyGrid + wx[i + 1]];
					glm::vec3 d = (off + h) - fp;
					f1 = std::min(f1, glm::dot(d, d));   // nearest feature (squared)
				}
		return glm::clamp(1.0f - std::sqrt(f1), 0.0f, 1.0f);
	};
	std::vector<uint16_t> d(static_cast<size_t>(n) * n * n * 2);
	const float inv = 1.0f / static_cast<float>(n);

	// Each voxel is fully independent and written to its own fixed index, so the
	// z-slabs run on plain threads (the parallel STL is unimplemented in
	// libc++/Apple Clang) without any effect on the bytes.
	auto fillSlab = [&](int z0, int z1) {
		for (int z = z0; z < z1; ++z)
			for (int y = 0; y < n; ++y)
				for (int x = 0; x < n; ++x)
				{
					const size_t idx = ((static_cast<size_t>(z) * n + y) * n + x) * 2;
					glm::vec3 uv((x + 0.5f) * inv, (y + 0.5f) * inv, (z + 0.5f) * inv);
					d[idx + 0] = static_cast<uint16_t>(
						glm::clamp(hash(glm::vec3(x, y, z)), 0.0f, 1.0f) * 65535.0f + 0.5f);
					d[idx + 1] = static_cast<uint16_t>(worley(uv) * 65535.0f + 0.5f);
				}
	};
	const int workers = n < 32 ? 1 : std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
	std::vector<std::thread> pool;
	for (int t = 1; t < workers; ++t)
		pool.emplace_back(fillSlab, n * t / workers, n * (t + 1) / workers);
	fillSlab(0, n / workers);
	for (std::thread& th : pool)
		th.join();
	return d;
}

} // namespace HE
