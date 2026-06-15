#pragma once
#include "../HE_RENDERING_API.h"

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

    // Day-night cycle: when enabled, the extractor drives the sun from the time
    // of day (0..1: 0.25 sunrise, 0.5 noon, 0.75 sunset, 0/1 midnight) instead of
    // the scene light's authored direction — it rotates the first directional
    // light (so shading + shadows follow) and dims it at night, and sets
    // RenderWorld::sunDirection for the sky. Render-time only; the scene ECS is
    // never modified.
    void setDayNight(bool enabled, float timeOfDay)
    {
        m_dayNight   = enabled;
        m_timeOfDay  = timeOfDay;
    }

private:
    bool  m_dayNight  = false;
    float m_timeOfDay = 0.5f;
};
