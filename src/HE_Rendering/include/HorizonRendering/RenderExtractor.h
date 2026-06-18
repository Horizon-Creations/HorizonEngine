#pragma once
#include "../HE_RENDERING_API.h"
#include <glm/vec3.hpp>

class HorizonWorld;
class RenderWorld;
struct EditorCameraOverride;

// Reads the ECS world each frame and fills a RenderWorld snapshot.
// This is the ONLY class in HorizonRendering that touches HorizonScene —
// it is compiled into the HorizonRendering DLL; backends only see the
// resulting RenderWorld.
class HE_RENDERING_API RenderExtractor {
public:
    // aspectRatio is needed to build the camera projection matrix and comes
    // from the backend's current swapchain size.
    // editorCam, when non-null and active, overrides the scene camera (used by
    // the editor scene view); its projection is built with aspectRatio so it
    // always matches the viewport.
    void extract(HorizonWorld& world, RenderWorld& outWorld, float aspectRatio,
                 const EditorCameraOverride* editorCam = nullptr);

    // Populate outWorld.uiObjects from UISystem::extract.
    // Called after extract() when viewport pixel dimensions are known.
    void extractUI(HorizonWorld& world, float vpWidth, float vpHeight, RenderWorld& outWorld);

    // Day-night cycle: when enabled, the extractor drives the sun from the time
    // of day (0..1: 0.25 sunrise, 0.5 noon, 0.75 sunset, 0/1 midnight) instead of
    // the scene light's authored direction — it rotates the first directional
    // light (so shading + shadows follow), adds a second moon light on the
    // opposite arc, and fades each out as its luminary sets. Sun/moon colour and
    // brightness are caller-supplied. Render-time only; the scene ECS is never
    // modified.
    void setDayNight(bool enabled, float timeOfDay,
                     const glm::vec3& sunColor, float sunIntensity,
                     const glm::vec3& moonColor, float moonIntensity,
                     float cloudCoverage)
    {
        m_dayNight       = enabled;
        m_timeOfDay      = timeOfDay;
        m_sunColor       = sunColor;
        m_sunIntensity   = sunIntensity;
        m_moonColor      = moonColor;
        m_moonIntensity  = moonIntensity;
        m_cloudCoverage  = cloudCoverage;
    }

private:
    bool      m_dayNight       = false;
    float     m_timeOfDay      = 0.5f;
    glm::vec3 m_sunColor       = glm::vec3(1.0f, 0.97f, 0.90f);
    float     m_sunIntensity   = 2.2f;
    glm::vec3 m_moonColor      = glm::vec3(0.55f, 0.65f, 0.95f);
    float     m_moonIntensity  = 0.66f;
    float     m_cloudCoverage  = 0.5f;
};
