#pragma once

class HorizonWorld;
class RenderWorld;

// Reads the ECS world each frame and fills a RenderWorld snapshot.
// This is the ONLY class in HorizonRendering that touches HorizonScene —
// it is compiled into the HorizonRendering DLL; backends only see the
// resulting RenderWorld.
class RenderExtractor {
public:
    // aspectRatio is needed to build the camera projection matrix and comes
    // from the backend's current swapchain size.
    void extract(HorizonWorld& world, RenderWorld& outWorld, float aspectRatio);
};
