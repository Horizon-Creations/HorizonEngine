#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <cstdint>

class HorizonWorld;

// PIMPL wrapper around Jolt PhysicsSystem.
// Keeps all Jolt headers out of the public API.
class PhysicsWorld
{
public:
    // Result of a single raycast. `hit` is false when no body was intersected.
    struct RaycastHit
    {
        bool      hit      = false;
        uint32_t  entityId = 0;      // raw entt::entity; cast to Entity for ECS use
        glm::vec3 point    = {};
        glm::vec3 normal   = {};
        float     distance = 0.0f;
    };

    PhysicsWorld();
    ~PhysicsWorld();

    // Build one body per entity that has RigidBodyComponent + TransformComponent.
    // Uses ColliderComponent shape when present; falls back to a box from scale.
    void initialize(HorizonWorld& world);

    // Advance the simulation by dt seconds, then write dynamic/kinematic body
    // positions and orientations back to TransformComponent.
    void step(HorizonWorld& world, float dt);

    // Cast a ray from `origin` along `direction` (need not be normalised) up to
    // `maxDistance` metres. Returns the closest hit or RaycastHit{hit=false}.
    RaycastHit raycast(const glm::vec3& origin,
                       const glm::vec3& direction,
                       float            maxDistance = 1000.0f) const;

    // Remove and destroy all physics bodies without touching the ECS.
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
