#pragma once
class HorizonWorld;

namespace ParticleSystem {
    // Advance all ParticleSystemComponents by dt seconds.
    // Emits new particles, integrates velocities, removes dead particles.
    void update(HorizonWorld& world, float dt);
}
