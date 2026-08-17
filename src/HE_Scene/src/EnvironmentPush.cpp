#include "HorizonScene/EnvironmentPush.h"
#include "HorizonScene/Components/EnvironmentComponent.h"
#include <algorithm>
#include <cmath>

namespace HE {

IRenderer::EnvironmentSettings makeEnvironmentSettings(EnvironmentComponent& env, float dt)
{
    // Auto-advance the day-night cycle (time flows with real time).
    if (env.dayNightCycle && env.autoAdvance && dt > 0.0f)
    {
        const float dayFrac = dt / std::max(env.cycleSeconds, 1.0f);
        env.timeOfDay += dayFrac;
        env.timeOfDay -= std::floor(env.timeOfDay); // wrap to [0,1)
        // Lunar cycle: the moon phase advances one full cycle per moonCycleDays day-night cycles.
        if (env.moonPhaseAuto)
        {
            env.moonPhase += dayFrac / std::max(env.moonCycleDays, 0.1f);
            env.moonPhase -= std::floor(env.moonPhase);
        }
    }

    return IRenderer::EnvironmentSettings{
        .dayNightCycle = env.dayNightCycle, .timeOfDay = env.timeOfDay,
        .sunColor = env.sunColor, .sunIntensity = env.sunIntensity,
        .moonColor = env.moonColor, .moonIntensity = env.moonIntensity,
        .moonPhase = env.moonPhase,
        .cloudCoverage = env.cloudCoverage,
        .fogDensity = env.fogDensity, .fogHeightFalloff = env.fogHeightFalloff,
        .auroraIntensity = env.auroraIntensity,
        .milkyWayIntensity = env.milkyWayIntensity, .nebulaIntensity = env.nebulaIntensity,
        .nebulaColor = env.nebulaColor, .nebulaColor2 = env.nebulaColor2,
        .nebulaColor3 = env.nebulaColor3, .nebulaSeed = env.nebulaSeed,
        .nebulaCoverage = env.nebulaCoverage,
        .nebulaQuality = env.nebulaQuality,
        .auroraColor = env.auroraColor,
        .auroraColorTop = env.auroraColorTop,
        .auroraHeight = env.auroraHeight, .auroraFragmentation = env.auroraFragmentation,
        .windDirection = env.windDirection, .windSpeed = env.windSpeed, .flash = env.flash,
        .wetness = env.wetness, .snowAmount = env.snowAmount, .rainAmount = env.rainAmount,
        .cloudMode = env.cloudMode, .cloudHeight = env.cloudHeight,
        .cloudQuality = env.cloudQuality, .lowResClouds = env.lowResClouds,
        .cloudShadows = env.cloudShadows, .cloudShadowStrength = env.cloudShadowStrength,
        .cloudStyle = env.cloudStyle, .cloudInterShadows = env.cloudInterShadows,
        .cloudEvolution = env.cloudEvolution,
        .cloudDensity = env.cloudDensity, .cloudFluffiness = env.cloudFluffiness,
        .cloudTint = env.cloudTint,
        .contrailAmount = env.contrailAmount,
        .cirrusAmount = env.cirrusAmount, .cirrusSeed = env.cirrusSeed,
        .godRays = env.godRays, .shootingStars = env.shootingStars, .lensFlare = env.lensFlare,
        .starBrightness = env.starBrightness, .starColor = env.starColor,
        .starSize = env.starSize, .starSizeVariation = env.starSizeVariation,
        .starGlow = env.starGlow, .starTwinkle = env.starTwinkle,
        .starDensity = env.starDensity};
}

} // namespace HE
