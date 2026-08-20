#include "PreviewPick.h"
#include <cmath>
#include <limits>

namespace PreviewPick
{

bool screenRay(const glm::mat4& viewProj, const glm::vec2& rectMin, const glm::vec2& rectSize,
               const glm::vec2& mouse, glm::vec3& outOrigin, glm::vec3& outDir)
{
	if (!(rectSize.x > 0.0f) || !(rectSize.y > 0.0f)) return false;

	const float u = (mouse.x - rectMin.x) / rectSize.x;
	const float v = (mouse.y - rectMin.y) / rectSize.y;
	// NDC: x right, y UP — the screen's y grows downward, hence the flip. Get
	// this wrong and picking works perfectly on the horizontal centre line and
	// selects the mirrored object everywhere else.
	const glm::vec4 ndcNear(2.0f * u - 1.0f, 1.0f - 2.0f * v, -1.0f, 1.0f);
	const glm::vec4 ndcFar (ndcNear.x, ndcNear.y, 1.0f, 1.0f);

	const glm::mat4 inv = glm::inverse(viewProj);
	glm::vec4 pNear = inv * ndcNear;
	glm::vec4 pFar  = inv * ndcFar;
	if (std::abs(pNear.w) < 1e-9f || std::abs(pFar.w) < 1e-9f) return false;
	pNear /= pNear.w;
	pFar  /= pFar.w;

	outOrigin = glm::vec3(pNear);
	outDir    = glm::vec3(pFar) - glm::vec3(pNear);
	return glm::dot(outDir, outDir) > 0.0f;
}

bool pick(const std::vector<Candidate>& candidates,
          const glm::vec3& rayOrigin, const glm::vec3& rayDir,
          std::uint32_t& outId, float* outT)
{
	float best = std::numeric_limits<float>::max();
	bool  found = false;
	for (const Candidate& c : candidates)
	{
		if (!c.box.isValid()) continue;
		// Ray into the candidate's own space, so a rotated box is an exact test
		// instead of the loose world-space box around it. The direction is
		// transformed as a VECTOR (w = 0): it carries the object's scale, which
		// is what keeps `t` comparable between candidates — it stays measured in
		// units of the world-space |rayDir|.
		const glm::mat4 invModel = glm::inverse(c.world);
		const glm::vec3 o = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
		const glm::vec3 d = glm::vec3(invModel * glm::vec4(rayDir,    0.0f));

		float t = 0.0f;
		if (!c.box.intersectRay(o, d, t)) continue;
		if (t >= best) continue;
		best  = t;
		outId = c.id;
		found = true;
	}
	if (found && outT) *outT = best;
	return found;
}

bool pickAtScreen(const std::vector<Candidate>& candidates, const glm::mat4& viewProj,
                  const glm::vec2& rectMin, const glm::vec2& rectSize, const glm::vec2& mouse,
                  std::uint32_t& outId)
{
	glm::vec3 ro(0.0f), rd(0.0f);
	if (!screenRay(viewProj, rectMin, rectSize, mouse, ro, rd)) return false;
	return pick(candidates, ro, rd, outId);
}

} // namespace PreviewPick
