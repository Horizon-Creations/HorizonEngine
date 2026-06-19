#pragma once
#include <glm/glm.hpp>

class HorizonWorld;
class PhysicsWorld;

namespace WeatherSystem
{
    // Advances the (first) WeatherComponent's transition toward its target preset and
    // writes the blended cloud coverage / fog density / wind speed into the World-root
    // EnvironmentComponent (the sun dims with cloud coverage automatically downstream).
    // Also simulates the camera-following precipitation volume (rain/snow) into the
    // WeatherComponent's particle pool, which RenderExtractor draws as billboards.
    // No-op when no WeatherComponent exists; the EnvironmentComponent may also be absent.
    // Runs every frame (editor preview + game), like AnimationSystem. cameraPos centres
    // the precipitation volume on the viewer. physics (optional) enables real collision:
    // the ground-height grid is sampled with downward raycasts when supplied, else it
    // falls back to WeatherComponent::groundLevel.
    void update(HorizonWorld& world, float dt, const glm::vec3& cameraPos = glm::vec3(0.0f),
                const PhysicsWorld* physics = nullptr);
}
