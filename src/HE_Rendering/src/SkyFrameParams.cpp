#include "HorizonRendering/SkyFrameParams.h"
#include <cmath>
#include <cstdlib>

namespace HE
{

glm::vec3 CloudWindVector(const IRenderer::EnvironmentSettings& env)
{
	const float rad = glm::radians(env.windDirection);
	return glm::vec3(std::sin(rad), 0.0f, -std::cos(rad)) * (env.windSpeed * 0.025f);
}

// HE_DUMP_SKYTIME pins the sky animation clock to a fixed second for a headless frame dump.
// Everything animated in the sky (cloud drift, aurora surges and streaming rays, shooting
// stars) is driven by this one clock, which is normally SDL_GetTicks() and therefore
// effectively constant across one-frame dump runs — so without an override the motion cannot
// be verified off-screen at all. Read once; unset in normal runs and then it costs nothing.
static float SkyTimeOverride(float wallTime)
{
	static const float s_fixed = []
	{
		const char* v = std::getenv("HE_DUMP_SKYTIME");
		return (v && *v) ? static_cast<float>(std::atof(v)) : -1.0f;
	}();
	return s_fixed >= 0.0f ? s_fixed : wallTime;
}

SkyFrameParams BuildSkyFrameParams(const IRenderer::EnvironmentSettings& env,
                                   const SkyFrameInputs&                 in)
{
	SkyFrameParams p;
	p.invViewProj    = in.invViewProj;
	p.sunDir         = glm::vec4(in.sunDir, in.hasMoonTexture ? 1.0f : 0.0f);
	p.sunColor       = glm::vec4(env.sunColor, env.moonPhase);
	p.params         = glm::vec4(env.timeOfDay, env.cloudCoverage, SkyTimeOverride(in.time), env.auroraIntensity);
	p.nebulaColor    = glm::vec4(env.nebulaColor, env.nebulaIntensity);
	p.auroraColor    = glm::vec4(env.auroraColor, env.milkyWayIntensity);
	p.wind           = glm::vec4(CloudWindVector(env), env.flash);
	p.cameraPos      = glm::vec4(in.cameraPos, static_cast<float>(env.cloudMode));
	p.cloud          = glm::vec4(env.cloudHeight, env.cloudDensity, env.cloudFluffiness, env.contrailAmount);
	p.cloudTint      = glm::vec4(env.cloudTint, env.cirrusAmount);
	p.cirrus         = glm::vec4(env.cirrusSeed, env.auroraHeight, env.auroraFragmentation, env.nebulaSeed);
	p.nebulaColor2   = glm::vec4(env.nebulaColor2, static_cast<float>(env.nebulaQuality));
	p.nebulaColor3   = glm::vec4(env.nebulaColor3, env.godRays);
	p.auroraColorTop = glm::vec4(env.auroraColorTop, env.shootingStars);
	p.starColor      = glm::vec4(env.starColor, env.starBrightness);
	p.star           = glm::vec4(env.starSize, env.starSizeVariation, env.starDensity, env.starGlow);
	p.star2          = glm::vec4(env.starTwinkle, static_cast<float>(env.cloudQuality),
	                             in.lowResClouds ? 1.0f : 0.0f, env.rainAmount);
	p.neb2           = glm::vec4(env.nebulaCoverage, static_cast<float>(env.cloudStyle),
	                             env.cloudInterShadows ? 1.0f : 0.0f, env.cloudEvolution);
	return p;
}

} // namespace HE
