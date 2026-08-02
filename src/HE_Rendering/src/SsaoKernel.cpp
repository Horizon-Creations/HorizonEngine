#include "HorizonRendering/SsaoKernel.h"

namespace HE
{

// ── Why every draw is its own named local ────────────────────────────────────
// The order in which function ARGUMENTS are evaluated is unspecified in C++, so
// `glm::vec3(rng.next(), rng.next(), rng.next())` hands the three draws to
// whichever component the compiler feels like: Clang consumed them left to
// right, MSVC and GCC right to left. That silently broke the one guarantee this
// file exists to provide — "every backend bakes the identical numbers" (see the
// header) — because the numbers then depended on the COMPILER, not the seed.
// macOS and Windows/Linux really did ship different SSAO kernels and noise
// tiles. Sequencing each draw into its own local is what makes the stream
// order part of the source instead of the toolchain.

std::vector<glm::vec3> BuildSSAOKernel(int n)
{
	SsaoRng rng{ 0x9E3779B9u };
	std::vector<glm::vec3> k(n);
	for (int i = 0; i < n; ++i)
	{
		const float sx = rng.next() * 2.0f - 1.0f;
		const float sy = rng.next() * 2.0f - 1.0f;
		const float sz = rng.next();
		glm::vec3 s(sx, sy, sz);
		const float scale = rng.next();
		s = glm::normalize(s) * scale;
		float t = static_cast<float>(i) / static_cast<float>(n);
		s *= 0.1f + 0.9f * t * t;   // accelerate the distribution toward the centre
		k[i] = s;
	}
	return k;
}

std::vector<glm::vec3> BuildSSAONoise(int n)
{
	SsaoRng rng{ 0x2545F491u };
	std::vector<glm::vec3> v(n);
	for (int i = 0; i < n; ++i)
	{
		// Same sequencing rule as above — these two came back swapped on
		// MSVC/GCC, which the cross-platform test caught as x and y exchanged.
		const float x = rng.next() * 2.0f - 1.0f;
		const float y = rng.next() * 2.0f - 1.0f;
		v[i] = glm::vec3(x, y, 0.0f);
	}
	return v;
}

std::vector<glm::vec4> BuildSSAONoiseRGBA(int n)
{
	// Deliberately re-derived from BuildSSAONoise rather than duplicating the RNG
	// loop: the padded and unpadded tiles can then never drift apart.
	const std::vector<glm::vec3> v = BuildSSAONoise(n);
	std::vector<glm::vec4> out(v.size());
	for (size_t i = 0; i < v.size(); ++i)
		out[i] = glm::vec4(v[i], 0.0f);
	return out;
}

} // namespace HE
