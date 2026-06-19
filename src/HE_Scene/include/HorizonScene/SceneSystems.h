#pragma once
#include <glm/glm.hpp>

class HorizonWorld;
class ContentManager;
class IRenderer;
class PhysicsWorld;

namespace SceneSystems
{
    // Runs the always-on visual / gameplay systems for one frame: terrain regen,
    // skeletal animation (clip / blend / state-machine / property), navigation,
    // weather, particles, foliage and LOD. Shared by the editor (preview every frame)
    // and the standalone game runtime so weather & friends behave identically in both.
    // Physics, scripts and audio are stepped separately by the caller — the editor gates
    // those on play-mode; the game runs them every frame.
    // physics (optional) enables real precipitation collision via the weather system's
    // ground-height grid (downward raycasts); nullptr falls back to a flat ground plane.
    void tick(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
              const glm::vec3& cameraPos, float dt, const PhysicsWorld* physics = nullptr);
}
