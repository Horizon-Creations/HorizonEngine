#pragma once
#include <glm/glm.hpp>

// ─── EnvironmentSettings ─────────────────────────────────────────────────────
// The sky / day-night / weather-appearance block every backend reads once per
// frame. Split out of IRenderer.h purely for size — it is ~60 fields and was the
// bulk of that header. It stays reachable as `IRenderer::EnvironmentSettings`
// (IRenderer includes this header and aliases the name), so no caller changed.
//
// Pushed by the editor / game via IRenderer::SetEnvironmentSettings. When
// dayNightCycle is on, the renderer's extractor drives the sun from timeOfDay
// (0..1: 0.25 sunrise, 0.5 noon, 0.75 sunset, 0/1 midnight) — moving the sky,
// the image-based ambient and the shadows together. Off = the scene's own
// directional light is used.
//
// KEEP IN SYNC: HE::makeEnvironmentSettings (HE_Scene/EnvironmentPush.cpp) is
// the single EnvironmentComponent → this-struct translation, and
// HE::BuildSkyFrameParams (HE_Rendering/SkyFrameParams.cpp) is the single
// this-struct → backend sky-constants translation. A new field needs both.
struct EnvironmentSettings
{
    // Master sky switch. False when the scene has no Sky entity (removed via the
    // Environment window): the backend skips the procedural sky pass entirely and
    // the background is left at the frame's clear colour. Defaults true so any
    // code path that forgets to set it keeps rendering the sky as before.
    bool  skyEnabled    = true;
    bool  dayNightCycle = false;
    float timeOfDay     = 0.5f; // noon
    // Sun & moon directional lights driven by the day-night cycle. Colour and
    // brightness are user-adjustable; each luminary is faded out as it sets.
    glm::vec3 sunColor      = glm::vec3(1.0f, 0.97f, 0.90f); // warm daylight
    float     sunIntensity  = 2.2f;
    glm::vec3 moonColor     = glm::vec3(0.55f, 0.65f, 0.95f); // cool moonlight
    float     moonIntensity = 0.66f;
    float     moonPhase     = 0.5f;  // 0/1 new … 0.5 full
    // Procedural cloud amount (0 = clear sky … 1 = full overcast). At full
    // overcast the sun/moon directional light is switched off (optimisation)
    // and replaced by a soft scattered ambient fill.
    float     cloudCoverage = 0.5f;
    // Atmospheric fog / aerial perspective. Distant scene geometry is blended
    // toward the procedural sky colour in its view direction, so it melts into
    // the horizon (and warms toward the sun at sunset). 0 density = no fog.
    // heightFalloff > 0 makes the fog pool near the ground and thin with
    // altitude (analytic exponential height fog); 0 = uniform distance fog.
    float     fogDensity      = 0.0f;
    float     fogHeightFalloff = 0.1f;
    // Night-sky aurora borealis intensity (0 = off). Drifting light ribbons
    // that sweep across the sky, drawn only at night.
    float     auroraIntensity = 0.0f;
    // Milky Way (dense star band) brightness, space-nebula intensity, and the
    // base colours for the nebula and the aurora ribbons. Stars + nebula
    // rotate with time-of-day to mimic Earth's rotation.
    float     milkyWayIntensity = 0.6f;
    float     nebulaIntensity   = 0.3f;
    glm::vec3 nebulaColor       = glm::vec3(0.36f, 0.60f, 1.00f);
    glm::vec3 nebulaColor2      = glm::vec3(1.00f, 0.60f, 0.28f);
    glm::vec3 nebulaColor3      = glm::vec3(0.90f, 0.30f, 0.16f);
    float     nebulaSeed        = 0.0f;
    float     nebulaCoverage    = 0.5f; // 0 = none .. 1 = nearly the whole band covered
    int       nebulaQuality      = 1;   // 0 Performance, 1 High, 2 Max (Metal + OpenGL)
    glm::vec3 auroraColor       = glm::vec3(0.25f, 0.95f, 0.50f);
    glm::vec3 auroraColorTop     = glm::vec3(0.62f, 0.26f, 0.95f);
    float     auroraHeight        = 0.18f;
    float     auroraFragmentation = 0.4f;
    // Cloud wind: the compass direction the clouds drift toward (degrees, 0 =
    // toward -Z/north, increasing clockwise) and a speed multiplier. The
    // backend turns these into a horizontal drift vector for the cloud noise.
    float     windDirection = 30.0f;
    float     windSpeed     = 1.0f;
    // Lightning flash brightness (0 = none … 1 = full strike). Driven by the
    // WeatherSystem during storms; added to the sky colour in the backend.
    float     flash         = 0.0f;
    // Ground response to weather (0..1). wetness darkens + glosses lit surfaces;
    // snowAmount lays white snow on up-facing surfaces. Read by the lit shader.
    float     wetness    = 0.0f;
    float     snowAmount = 0.0f;
    float     rainAmount = 0.0f;   // drives the sky rainbow (rain + sun) — Metal/OpenGL sky pass
    // Cloud render mode (OpenGL): 0 = sky-dome (default), 1 = 3D volumetric clouds
    // anchored in the world so they parallax as the camera moves. cloudHeight = the
    // 3D layer's height above the camera in world units. Other backends ignore these.
    int       cloudMode   = 0;
    float     cloudHeight = 200.0f;
    // Cloud raymarch quality (perf knob): 0 Low, 1 Medium, 2 High — scales step counts.
    int       cloudQuality = 1;
    bool      lowResClouds = false; // quarter-res cloud pre-pass + upsample (perf; default off)
    // Cloud appearance (OpenGL 3D path): density scales opacity, fluffiness
    // drives the cauliflower erosion, tint colours the clouds.
    float     cloudDensity    = 1.0f;
    float     cloudFluffiness = 0.6f;
    glm::vec3 cloudTint       = glm::vec3(1.0f);
    // Contrails: scattered vapour-trail lines that fill an empty daytime sky.
    float     contrailAmount  = 0.0f;
    // Thin high cirrus clouds: amount = cover/brightness, seed re-rolls the pattern.
    float     cirrusAmount    = 0.0f;
    float     cirrusSeed      = 0.0f;
    float     godRays         = 0.0f;   // crepuscular sun-shaft strength — Metal/OpenGL sky pass
    float     shootingStars   = 0.0f;   // meteor frequency (0 = none) — Metal/OpenGL night sky
    float     lensFlare       = 0.0f;   // camera sun lens-flare strength (0 = off) — post-process overlay
    // Star field brightness + colour tint, overall size and size variation.
    float     starBrightness    = 1.0f;
    glm::vec3 starColor         = glm::vec3(1.0f);
    float     starSize          = 1.0f;
    float     starSizeVariation = 0.5f;
    float     starGlow          = 1.0f;
    float     starTwinkle       = 0.6f;
    float     starDensity       = 0.5f;
};
