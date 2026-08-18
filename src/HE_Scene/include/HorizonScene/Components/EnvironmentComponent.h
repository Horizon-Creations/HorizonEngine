#pragma once
#include <Math/Math.h>

// Scene-wide environment / sky settings, authored on the "Sky" entity — an
// ordinary, deletable scene entity in the Outliner, added/removed via the editor's
// Environment window (View menu); a scene without one has no sky at all. Consumers
// find it with HorizonWorld::environmentEntity() instead of looking at the root.
// Persisted with the scene (the SceneSerializer writes/reads it like any other
// component); scenes from before the split carried it on the World root and are
// moved onto a Sky entity on load by HorizonWorld::migrateLegacyRootEnvironment().
// The editor edits it in the Sky node's Details panel and pushes it to the renderer
// each frame via IRenderer::SetEnvironmentSettings. Defaults match the previous
// global editor defaults so existing scenes look unchanged.
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
    // ABSOLUTE world altitude of the cloud deck (its base; the layer is
    // 1.5x this thick). Absolute, not camera-relative: that is what lets the
    // camera climb INTO and ABOVE the clouds and look down on them. A scene
    // whose playfield sits high up must raise this accordingly, or its ground
    // ends up above the clouds.
    float cloudHeight = 200.0f;
    // Cloud raymarch quality (performance knob): 0 = Low, 1 = Medium, 2 = High.
    // Scales the view-ray step count and sun light-march steps in both cloud paths.
    // Lower = cheaper (helps on integrated GPUs / Apple Silicon Air); default Medium.
    int   cloudQuality = 1;
    // Cloud shadows: the procedural cloud layer casts moving shadows onto the
    // scene (projected along the sun; one small transmittance-map pass per
    // frame). Darkens only the sun/moon directional light — ambient is
    // untouched, so shadowed ground reads as "under a cloud", not night.
    bool  cloudShadows        = true;
    float cloudShadowStrength = 0.7f;   // 0 = invisible … 1 = full darkening
    // Cloud look (3D volumetric mode): 0 = Classic (the original soft look,
    // byte-identical to before the option existed), 1 = Realistic — HZD-style
    // Perlin-Worley base shapes (connected cauliflower formations), flat cloud
    // bases, sharper silhouettes, multi-scattered sun lighting with bright tops
    // over blue-grey bellies and a silver lining toward the sun.
    int   cloudStyle = 1;
    // Clouds shade EACH OTHER (3D mode): extends the sun light-march beyond the
    // cloud's own body so a tall neighbour tower darkens the clouds behind it.
    // Slightly more expensive; step count scales with cloudQuality.
    bool  cloudInterShadows = true;
    // Cloud life (3D mode, Realistic style): how alive the shapes are over time.
    // 0 = frozen shapes that only drift with the wind; 1 = natural — cauliflower
    // lobes boil upward (convection), tops lean downwind (wind shear), and a
    // slow formation field makes clouds grow, tower and dissolve as they drift;
    // 2 = time-lapse. Scales the evolution SPEED, not the wind drift.
    float cloudEvolution = 1.0f;
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

    // Crepuscular rays (god-rays): bright shafts of sunlight streaming through gaps in
    // the cloud layer toward the sun. 0 = off. Computed in the sky shader (no extra
    // full-screen pass), gated to a cone around the sun by day — kind to the frame
    // budget. Strongest with broken cloud cover (needs cloudCoverage > 0 to have gaps).
    float     godRays = 0.0f;   // 0 none … 1 strong shafts

    // Shooting stars / meteors: occasional bright streaks across the night sky. 0 = none,
    // higher = more frequent (and more concurrent). Night-only; deterministic from the sky
    // clock so they animate smoothly and reproduce in headless captures.
    float     shootingStars = 0.0f;   // 0 none … 1 frequent meteors

    // Camera lens flare for the sun (0 = off): a post-process overlay — bright core at the
    // sun's screen position, a chain of ghost discs along the sun→centre axis, and a halo
    // ring. Fades out when the sun is off-screen, below the horizon, behind cloud, or
    // occluded by geometry. A camera artifact (opt-in), not an eye-view phenomenon.
    float     lensFlare = 0.0f;   // 0 off … 1 strong

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
    glm::vec3 nebulaColor       = glm::vec3(0.36f, 0.60f, 1.00f); // colour 1: interior synchrotron veil (cool blue)
    glm::vec3 nebulaColor2      = glm::vec3(1.00f, 0.60f, 0.28f); // colour 2: filament cage (gold/amber regions)
    glm::vec3 nebulaColor3      = glm::vec3(0.90f, 0.30f, 0.16f); // colour 3: filament cage (rust/red regions)
    float     nebulaSeed        = 0.0f;                           // randomisation seed
    float     nebulaCoverage    = 0.5f;                           // sky coverage: 0 = none .. 1 = nearly the whole band
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
