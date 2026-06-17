#pragma once
#include <memory>

class HorizonWorld;

// PIMPL wrapper around Jolt PhysicsSystem.
// Keeps all Jolt headers out of the public API.
class PhysicsWorld
{
public:
    PhysicsWorld();
    ~PhysicsWorld();

    // Build one box body per entity that has RigidBodyComponent + TransformComponent.
    // Half-extents are derived from TransformComponent::scale * 0.5.
    void initialize(HorizonWorld& world);

    // Advance the simulation by dt seconds, then write dynamic/kinematic body
    // positions and orientations back to TransformComponent.
    void step(HorizonWorld& world, float dt);

    // Remove and destroy all physics bodies without touching the ECS.
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
