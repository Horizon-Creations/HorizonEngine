#pragma once
#include <glm/glm.hpp>
class HorizonWorld;

namespace ParticleSystem {
    // Advance all ParticleSystemComponents by dt seconds.
    // Emits new particles, integrates velocities, removes dead particles.
    // cameraPos positions camera-following volume emitters (e.g. precipitation).
    void update(HorizonWorld& world, float dt, const glm::vec3& cameraPos = glm::vec3(0.0f));
}
