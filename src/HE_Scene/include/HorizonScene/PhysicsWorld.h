#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <cstdint>
#include <vector>

class HorizonWorld;

// PIMPL wrapper around Jolt PhysicsSystem.
// Keeps all Jolt headers out of the public API.
class PhysicsWorld
{
public:
    // A single contact event between two entities (identified by raw entity handle).
    struct CollisionEvent
    {
        uint32_t entityA = 0;
        uint32_t entityB = 0;
    };

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

    // "No entity" for the ignore parameters below. A real entity id can be 0,
    // so the sentinel has to be a value the allocator never hands out.
    static constexpr uint32_t kNoEntity = 0xFFFFFFFFu;

    // Cast a ray from `origin` along `direction` (need not be normalised) up to
    // `maxDistance` metres. Returns the closest hit or RaycastHit{hit=false}.
    //
    // `ignoreEntityId` skips that entity's body — without it a ray fired from
    // something's own position reports that something, which is never what the
    // caller meant. Triggers ARE reported, as they always were; callers that
    // care check the hit entity.
    RaycastHit raycast(const glm::vec3& origin,
                       const glm::vec3& direction,
                       float            maxDistance = 1000.0f,
                       uint32_t         ignoreEntityId = kNoEntity) const;

    // Sweep a sphere of `radius` from `origin` along `direction` up to
    // `maxDistance` metres, and report the first thing it touches.
    //
    // This is what a camera boom needs and a ray cannot give it: a ray is a line,
    // so it slips past wall corners that the camera's near plane then cuts
    // through. The sphere stops its CENTRE one radius short of the surface, which
    // is exactly the clearance a camera wants.
    //
    // Unlike raycast, this one skips TRIGGERS. A sweep is asking "what would
    // block me", and a trigger volume blocks nothing — a checkpoint between the
    // player and the camera would otherwise yank the view in.
    RaycastHit sphereCast(const glm::vec3& origin,
                          const glm::vec3& direction,
                          float            radius,
                          float            maxDistance,
                          uint32_t         ignoreEntityId = kNoEntity) const;

    // Every entity whose body overlaps a sphere at `center`. This is the query
    // an explosion and a melee swing are built from: everything in range in one
    // call, instead of a fan of rays that misses whatever sits between them.
    //
    // NOT pollOverlapEnter(): that drains trigger CONTACTS the simulation
    // produced during a step, this asks about the world as it stands right now
    // and needs no trigger volume anywhere.
    //
    // Sensors ARE reported, like raycast and unlike sphereCast — this is a
    // query ("what is here"), not a sweep ("what would block me"). A
    // CharacterVirtual is not a body and only appears through its kinematic
    // collision proxy, which is what EntityHost gives every PlayerCharacter.
    std::vector<uint32_t> overlapSphere(const glm::vec3& center,
                                        float            radius,
                                        uint32_t         ignoreEntityId = kNoEntity) const;

    // ── Rigid bodies: the write half ─────────────────────────────────────────
    // What makes a crate pushable from a script. Each addresses the body built
    // by initialize() for the entity's RigidBodyComponent, and each returns
    // false AND LOGS when there is none, or when the body's motion type cannot
    // take the operation — a silent no-op here reads as "physics is broken" to
    // whoever wrote the script, and that is the expensive kind of bug.
    //
    // A force is continuous: Jolt accumulates it, consumes it in the next
    // step() and clears it, so a sustained push has to be applied every frame.
    // An impulse lands once and changes the velocity immediately. Both need a
    // DYNAMIC body — a static or kinematic one has no solver state to push.
    // Sleeping bodies are woken by Jolt itself for all three (BodyInterface
    // guards on IsDynamic and activates), so a settled crate still reacts.
    bool addForce(uint32_t entityId, const glm::vec3& force);      // Newtons, at the centre of mass
    bool addImpulse(uint32_t entityId, const glm::vec3& impulse);  // kg·m/s, at the centre of mass
    bool addTorque(uint32_t entityId, const glm::vec3& torque);    // N·m around the world axes

    // Linear velocity of whatever the entity moves by, in m/s.
    //
    // ONE pair for characters and rigid bodies, dispatching on which of the two
    // the entity has, rather than a second pair beside setCharacterVelocity.
    // The reason is that both mean the same thing to a caller ("move at this
    // speed") and the engine already knows which representation an entity moves
    // by, so making the caller pick would only be a way to pick wrong. The
    // CHARACTER wins when an entity has both — the normal case, since every
    // PlayerCharacter carries a character controller plus a kinematic collision
    // proxy — and that is exactly what this call did when it was
    // character-only, so no existing project changes meaning by gaining the
    // rigid-body half.
    bool      setVelocity(uint32_t entityId, const glm::vec3& velocity);
    glm::vec3 getVelocity(uint32_t entityId) const;

    // Set the movement velocity for a CharacterController entity (m/s).
    // Has no effect if the entity has no active character controller.
    void setCharacterVelocity(uint32_t entityId, const glm::vec3& velocity);

    // Returns true if the character's feet are on solid ground.
    bool isCharacterGrounded(uint32_t entityId) const;

    // World gravity in m/s², default (0, -9.81, 0). Rigid bodies only: a
    // character controller falls by its own CharacterControllerComponent::
    // gravity, which is per character on purpose (a floaty player in ordinary
    // gravity is a design choice, not a bug).
    void      setGravity(const glm::vec3& gravity);
    glm::vec3 gravity() const;

    // Drain and return all collision-enter events recorded since the last call.
    // Events are generated by the Jolt contact listener during step(); safe to call
    // every frame whether or not physics was stepped. Reported once per body pair,
    // not once per touching sub shape.
    std::vector<CollisionEvent> pollCollisionEnter();

    // Drain and return all collision-exit events recorded since the last call.
    // Mirrors pollCollisionEnter(): exactly one event per body pair, emitted when
    // the last contact between the two bodies goes away. Pairs whose bodies were
    // destroyed via clear() do not produce an exit event.
    std::vector<CollisionEvent> pollCollisionExit();

    // The same two, for contacts where at least one side is a TRIGGER
    // (ColliderComponent::isTrigger — the body is created as a Jolt sensor, so
    // it passes bodies through and only reports). A contact lands in exactly one
    // of the two pairs, decided when it begins, so a trigger never also shows up
    // as a blocking hit.
    std::vector<CollisionEvent> pollOverlapEnter();
    std::vector<CollisionEvent> pollOverlapExit();

    // The fixed step both apps drive the simulation at. It lives here rather
    // than once per application because a game that simulates at a different
    // rate than the editor previewed it is not the same game — and two copies
    // of a number like this drift the moment one of them is tuned.
    static constexpr float kFixedDt = 1.0f / 60.0f;

    // Remove and destroy all physics bodies without touching the ECS.
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
