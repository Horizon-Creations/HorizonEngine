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
    // Lunar phase: 0/1 = new, 0.25 = first quarter, 0.5 = full, 0.75 = last quarter.
    float     moonPhase     = 0.5f;   // full by default
    bool      moonPhaseAuto = true;   // advance the phase with the day-night cycle
    float     moonCycleDays = 29.5f;  // days (day-night cycles) per full lunar cycle

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
    // Cloud raymarch quality (performance knob): 0 = Low, 1 = Medium, 2 = High.
    // Scales the view-ray step count and sun light-march steps in both cloud paths.
    // Lower = cheaper (helps on integrated GPUs / Apple Silicon Air); default Medium.
    int   cloudQuality = 1;
    // Low-resolution cloud pass (performance): raymarch the clouds at quarter resolution
    // into an offscreen buffer, then bilinear-upsample + composite. Big win in open-sky
    // views (clouds are the dominant per-pixel sky cost). Default OFF = the proven inline
    // path (so it can never regress); turn ON and A/B with F9 to measure the win. Metal first.
    bool  lowResClouds = false;
    // Cloud appearance knobs (so the look can be tweaked without re-rolling the
    // pattern): density scales opacity/thickness, fluffiness drives the cauliflower
    // erosion (higher = puffier, more broken-up billows), tint colours the clouds.
    float     cloudDensity    = 1.0f;   // 0.2 wispy … 2 thick/dense
    float     cloudFluffiness = 0.6f;   // 0 smooth sheet … 1 very billowy
    glm::vec3 cloudTint       = glm::vec3(1.0f); // multiplied into the cloud colour

    // Contrails (Kondensstreifen): scattered vapour-trail lines that fill an otherwise
    // empty daytime sky. 0 = none. Independent of the cloud layer.
    float     contrailAmount = 0.0f;
    // Thin high cirrus clouds: wispy fibrous streaks high in the sky. amount = how much
    // cover/brightness, seed re-rolls the pattern. 0 = none.
    float     cirrusAmount = 0.0f;
    float     cirrusSeed   = 0.0f;

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
    float     nebulaIntensity   = 0.3f;
    glm::vec3 nebulaColor       = glm::vec3(0.42f, 0.45f, 0.92f); // colour 1 (cool regions)
    glm::vec3 nebulaColor2      = glm::vec3(0.85f, 0.40f, 1.00f); // colour 2 (mid regions)
    glm::vec3 nebulaColor3      = glm::vec3(1.00f, 0.52f, 0.72f); // colour 3 (warm regions)
    float     nebulaSeed        = 0.0f;                           // randomisation seed
    int       nebulaQuality      = 1;                             // 0 = Performance (cheap), 1 = High (detailed), 2 = Max (most detail)
    glm::vec3 auroraColor       = glm::vec3(0.25f, 0.95f, 0.50f); // lower/base colour (green)
    glm::vec3 auroraColorTop     = glm::vec3(0.62f, 0.26f, 0.95f); // upper colour (purple)
    // Aurora band elevation (0 low/horizon … 1 high) and fragmentation
    // (0 = clean continuous band … 1 = broken into patches).
    float     auroraHeight        = 0.18f;
    float     auroraFragmentation = 0.4f;
    // Star field brightness multiplier + overall colour tint (per-star warm/cool
    // variation is preserved; this tints/scales the whole field). starSize scales the
    // overall star size; starSizeVariation controls how much sizes differ (0 = uniform,
    // 1 = wide small→large spread).
    float     starBrightness    = 1.0f;
    glm::vec3 starColor         = glm::vec3(1.0f);
    float     starSize          = 1.0f;
    float     starSizeVariation = 0.5f;
    // Glow halo around stars (0 = crisp points only, higher = more glow) and twinkle
    // amount (0 = steady, 1 = strong blinking).
    float     starGlow    = 1.0f;
    float     starTwinkle = 0.6f;
    float     starDensity = 0.5f;   // amount of stars (0 = few … 1 = many)
};
