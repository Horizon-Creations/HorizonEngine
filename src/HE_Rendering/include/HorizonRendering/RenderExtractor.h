#pragma once

class HorizonWorld;
class RenderWorld;
struct EditorCameraOverride;

// Reads the ECS world each frame and fills a RenderWorld snapshot.
// This is the ONLY class in HorizonRendering that touches HorizonScene —
// it is compiled into the HorizonRendering DLL; backends only see the
// resulting RenderWorld.
class RenderExtractor {
public:
    // aspectRatio is needed to build the camera projection matrix and comes
    // from the backend's current swapchain size.
    // editorCam, when non-null and active, overrides the scene camera (used by
    // the editor scene view); its projection is built with aspectRatio so it
    // always matches the viewport.
    void extract(HorizonWorld& world, RenderWorld& outWorld, float aspectRatio,
                 const EditorCameraOverride* editorCam = nullptr);
};
