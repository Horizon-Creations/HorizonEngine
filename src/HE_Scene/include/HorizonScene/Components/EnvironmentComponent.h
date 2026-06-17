#pragma once
#include <Math/Math.h>

// Scene-wide environment / sky settings, authored on the World root entity and
// persisted with the scene (the SceneSerializer writes/reads it like any other
// component). The editor edits it in the World node's Details panel and pushes it
// to the renderer each frame via IRenderer::SetEnvironmentSettings. Defaults match
// the previous global editor defaults so existing scenes look unchanged.
struct EnvironmentComponent
{
    // Day-night cycle: timeOfDay 0..1 (0.25 sunrise, 0.5 noon, 0.75 sunset, 0/1
    // midnight) drives the sun, sky, image-based ambient and shadows together.
    bool  dayNightCycle = false;
    float timeOfDay      = 0.5f;
    bool  autoAdvance    = false;    // advance timeOfDay automatically
    float cycleSeconds   = 120.0f;   // real seconds for one full day

    // Sun & moon directional lights (colour + brightness).
    glm::vec3 sunColor      = glm::vec3(1.0f, 0.97f, 0.90f);
    float     sunIntensity  = 2.2f;
    glm::vec3 moonColor     = glm::vec3(0.55f, 0.65f, 0.95f);
    float     moonIntensity = 0.66f;

    // Procedural clouds: coverage 0 = clear … 1 = overcast, plus wind drift.
    float cloudCoverage = 0.5f;
    float windDirection = 30.0f;     // degrees, 0 = toward -Z/north, clockwise
    float windSpeed     = 1.0f;

    // Atmospheric fog / aerial perspective (0 density = off; height falloff pools
    // the fog near the ground).
    float fogDensity      = 0.0f;
    float fogHeightFalloff = 0.1f;

    // Night sky: aurora ribbons, Milky-Way band and the space nebula.
    float     auroraIntensity   = 0.0f;
    float     milkyWayIntensity = 0.6f;
    float     nebulaIntensity   = 0.5f;
    glm::vec3 nebulaColor       = glm::vec3(0.42f, 0.45f, 0.92f);
    glm::vec3 auroraColor       = glm::vec3(0.25f, 0.95f, 0.50f);
};
