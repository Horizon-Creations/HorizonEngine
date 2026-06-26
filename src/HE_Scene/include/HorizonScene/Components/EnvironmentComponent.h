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
    // Cloud render mode (OpenGL backend): 0 = sky-dome clouds (the default; clouds sit
    // on the sky hemisphere — cheap, but no parallax), 1 = 3D volumetric clouds anchored
    // in the world so they parallax / shift as the camera moves through the scene.
    // cloudHeight is how far above the camera the 3D layer sits, in world units — tune
    // it to your world's scale (bigger world = larger value).
    int   cloudMode   = 0;
    float cloudHeight = 200.0f;

    // Atmospheric fog / aerial perspective (0 density = off; height falloff pools
    // the fog near the ground).
    float fogDensity      = 0.0f;
    float fogHeightFalloff = 0.1f;

    // Precipitation + ground response (0..1). The single source of truth for both
    // the weather-particle systems and the terrain shading — set by a WeatherPreset
    // when one is applied, or dialled in by hand (the sliders are always live). The
    // particle path renders the dominant of rain/snow; wetness darkens + glosses lit
    // surfaces, snowAmount lays white on up-facing ones.
    float rainAmount = 0.0f;   // rain density → velocity-streak billboards
    float snowAmount = 0.0f;   // snow density → flake billboards + ground snow cover
    float wetness    = 0.0f;   // wet-surface darkening + specular boost

    // Lightning flash (0..1, runtime only — driven by the WeatherSystem during storms,
    // never serialized). Brightens the sky shader for a brief strike.
    float flash = 0.0f;

    // Night sky: aurora ribbons, Milky-Way band and the space nebula.
    float     auroraIntensity   = 0.0f;
    float     milkyWayIntensity = 0.6f;
    float     nebulaIntensity   = 0.5f;
    glm::vec3 nebulaColor       = glm::vec3(0.42f, 0.45f, 0.92f);
    glm::vec3 auroraColor       = glm::vec3(0.25f, 0.95f, 0.50f);
};
