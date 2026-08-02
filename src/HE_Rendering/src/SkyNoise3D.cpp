#include "HorizonRendering/SkyNoise3D.h"
#include <Math/Math.h>
#include <algorithm>
#include <cmath>

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
	auto worley = [&](glm::vec3 uv) {
		glm::vec3 pc = uv * static_cast<float>(kWorleyGrid);
		glm::vec3 id = glm::floor(pc);
		glm::vec3 fp = pc - id;
		float f1 = 1e9f;
		for (int k = -1; k <= 1; ++k)
			for (int j = -1; j <= 1; ++j)
				for (int i = -1; i <= 1; ++i)
				{
					glm::vec3 off(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k));
					glm::vec3 wrapped = glm::mod(id + off, static_cast<float>(kWorleyGrid)); // seamless tile
					glm::vec3 d = (off + hash3(wrapped)) - fp;
					f1 = std::min(f1, glm::dot(d, d));   // nearest feature (squared)
				}
		return glm::clamp(1.0f - std::sqrt(f1), 0.0f, 1.0f);
	};
	std::vector<uint16_t> d(static_cast<size_t>(n) * n * n * 2);
	const float inv = 1.0f / static_cast<float>(n);

	// Serial nested loops (one-time init): each voxel is fully independent.
	// Portable across compilers without the parallel STL (<execution> is
	// unimplemented in libc++/Apple Clang).
	for (int z = 0; z < n; ++z)
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				const size_t idx = ((static_cast<size_t>(z) * n + y) * n + x) * 2;
				glm::vec3 uv((x + 0.5f) * inv, (y + 0.5f) * inv, (z + 0.5f) * inv);
				d[idx + 0] = static_cast<uint16_t>(
					glm::clamp(hash(glm::vec3(x, y, z)), 0.0f, 1.0f) * 65535.0f + 0.5f);
				d[idx + 1] = static_cast<uint16_t>(worley(uv) * 65535.0f + 0.5f);
			}
	return d;
}

} // namespace HE
