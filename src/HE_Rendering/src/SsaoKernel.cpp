#include "HorizonRendering/SsaoKernel.h"

namespace HE
{

std::vector<glm::vec3> BuildSSAOKernel(int n)
{
	SsaoRng rng{ 0x9E3779B9u };
	std::vector<glm::vec3> k(n);
	for (int i = 0; i < n; ++i)
	{
		glm::vec3 s(rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f, rng.next());
		s = glm::normalize(s) * rng.next();
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
		v[i] = glm::vec3(rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f, 0.0f);
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
