#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <cstdint>
#include <vector>

class HorizonWorld;
class ContentManager;

// PIMPL wrapper around Jolt PhysicsSystem.
// Keeps all Jolt headers out of the public API.
//
// SPACE CONVENTION, and it decides the meaning of nearly every position below:
// EVERY pose this class exchanges with Jolt — and every position and rotation in
// its own public API — is a WORLD pose. TransformComponent stores a LOCAL one,
// so both directions are converted through HE::TransformHierarchy (the composing
// worldMatrixOf()/localPositionForWorld(), never TransformComponent::worldMatrix,
// which is only as fresh as the last propagateTransforms() and is the identity
// for an entity spawned this frame).
//
// The two are the same thing for a top-level entity, which is why the difference
// went unnoticed: it shows up on a prefab's children and on the contents of an
// additively loaded zone, where the meshes are drawn through the hierarchy and
// the colliders were not.
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

    // Where triangle meshes come from. ColliderShape::Mesh and ::ConvexHull are
    // built from the entity's own mesh asset, which only the ContentManager can
    // hand out — everything else (box, sphere, capsule, terrain height field)
    // needs nothing but the ECS.
    //
    // Nullable, and null is not fatal: those two shapes then log and fall back to
    // a box, because a body that is the wrong shape is still a body, while an
    // entity with no body at all is the failure this whole class was audited for.
    // Both applications set it right after constructing the world.
    void setContentManager(ContentManager* content);

    // Build one body per entity that has RigidBodyComponent + TransformComponent,
    // plus one character controller per CharacterControllerComponent, plus a
    // static height field for every TerrainComponent that has no rigid body of
    // its own (nothing authors one, and a landscape you fall through is not a
    // landscape). Uses ColliderComponent shape when present; falls back to a box
    // from scale.
    //
    // This is the BULK build for a scene that is being started. It is not the
    // only way in: addEntity()/addEntityTree() below build the same
    // representation for one entity at a time, which is what a runtime spawn
    // needs. initialize() calls clear() first, so anything added before it is
    // discarded.
    void initialize(HorizonWorld& world);

    // Advance the simulation by dt seconds, then write dynamic/kinematic body
    // positions and orientations back to TransformComponent — as LOCAL poses,
    // since what Jolt reports is a world one (see the space convention above).
    //
    // Also reaps: an entity whose handle is no longer valid has its body and
    // character destroyed here. Nothing notifies this class when a scene deletes
    // an entity — not HorizonWorld::destroyEntity, not the outliner, not
    // entity.destroy from a script — so a sweep at the one point that sees every
    // representation every frame is the only cleanup that cannot be forgotten by
    // a caller. removeEntity() stays the immediate way; this is the safety net,
    // and it costs at most one frame of ghost collider.
    void step(HorizonWorld& world, float dt);

    // ── Runtime composition ──────────────────────────────────────────────────
    // Physics used to exist only for entities that were in the scene when it
    // started: no projectile, pickup, spawned enemy or piece of debris ever
    // collided, and a destroyed entity left its body behind as an invisible
    // wall. These are the way in and out at runtime.
    //
    // THREADING/REENTRANCY: call them from game code, never from inside a
    // physics callback. Contact callbacks only buffer events (they are drained
    // by pollCollisionEnter() and friends), so today every caller already
    // satisfies this — but a spawn issued *during* PhysicsSystem::Update would
    // add a body to a world that is mid-solve. Calling them during step()'s
    // write-back phase IS safe: it iterates a snapshot of the ids on purpose.

    // Build the physics representation of ONE entity from the components it has
    // right now: a body for RigidBodyComponent, a character for
    // CharacterControllerComponent, both when it has both (the normal case for a
    // player). Idempotent — an existing representation is torn down first, so
    // calling this after changing a collider rebuilds it rather than leaking the
    // old body. Returns false when the entity has nothing to build.
    bool addEntity(HorizonWorld& world, uint32_t entityId);

    // addEntity for a whole subtree, root included, and the count that was
    // built. This is what a spawn needs: a prefab such as a PlayerCharacter
    // brings child entities with their own colliders.
    int addEntityTree(HorizonWorld& world, uint32_t rootEntityId);

    // Destroy the entity's body and/or character. Silent no-op when it has
    // neither. Also drops the entity's pending contact bookkeeping, so a removal
    // never produces an exit event naming an entity that is already gone.
    void removeEntity(uint32_t entityId);
    int  removeEntityTree(HorizonWorld& world, uint32_t rootEntityId);

    // TELEPORT — not a movement. Sets the physics representation's position
    // outright and writes the matching LOCAL value into TransformComponent, so
    // the rest of the frame (camera, render extraction, scripts) sees one
    // consistent pose instead of the old one until the next step.
    //
    // `position` and `rotation` are WORLD — see the space convention above. What
    // lands in the TransformComponent is the local pose that PUTS the entity
    // there, which is the same value for a top-level entity and the parent's
    // offset removed for anything else.
    //
    // Jolt is written DIRECTLY rather than via the transform, because the two
    // applications run scripts on opposite sides of the step (the game before
    // it, the editor after it) — a teleport that relied on frame order would
    // behave differently in a packaged build than in Play-In-Editor.
    //
    // When an entity has both a character and a body, the CHARACTER is what
    // "the position" means and the kinematic proxy is hard-set to follow it —
    // the same precedence setVelocity()/getVelocity() already use.
    //
    // setPosition moves and nothing else: ROTATION IS NOT TOUCHED, not even
    // read. A character's Jolt rotation is never written by anything (the
    // transform owns its facing), so reading it back would hand a respawning
    // player the direction they faced when the level started.
    //
    // Velocity is left alone unless resetVelocity is set, which is what a
    // respawn wants (a player teleported to a checkpoint should not arrive with
    // the fall speed that killed them). Returns false AND LOGS for an entity
    // with no physics representation; use transform.setPosition for those.
    bool setPosition(uint32_t entityId, const glm::vec3& position, bool resetVelocity = false);
    bool setTransform(uint32_t entityId, const glm::vec3& position, const glm::quat& rotation,
                      bool resetVelocity = false);

    // Does this entity have a body or a character controller? For diagnostics,
    // for tests, and for game code that wants to ask before pushing.
    bool hasPhysics(uint32_t entityId) const;

    // Specifically a CHARACTER CONTROLLER, which hasPhysics above cannot answer:
    // it is an OR, and a PlayerCharacter carries both a controller and a
    // kinematic proxy body. An entity whose character failed to build but whose
    // body did passes hasPhysics and then finds setCharacterVelocity a silent
    // no-op — which is the difference between "this NPC will not move" and "this
    // NPC will not move and nothing will say so".
    bool hasCharacter(uint32_t entityId) const;

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
    // What makes a crate pushable from a script. Each addresses the body the
    // entity's RigidBodyComponent was built into — by initialize() at scene
    // start or by addEntity() at runtime — and each returns
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

    // Jump: replace the character's vertical velocity with an upward one, once.
    // The first takes the speed from CharacterControllerComponent::jumpSpeed,
    // the second overrides it (a charged jump, a low hop through a gap). Returns
    // whether the character actually left the ground, so `if (jump()) playSound()`
    // does the obvious thing.
    //
    // Takes no HorizonWorld for the same reason setPosition() does not: a jump is
    // written from game code that knows an entity id and nothing else. The world
    // remembered by initialize()/step() is used to reach the component.
    //
    // WHEN IT IS ALLOWED: on the ground — the same answer movement.isGrounded
    // gives, read from the same field, so a script that gates its own jump on
    // that line can never disagree with this one — OR within a short COYOTE
    // WINDOW after walking off a ledge (see CharacterControllerComponent::
    // airTime). The window is an engine constant rather than an authored field:
    // it is a feel-fix for the frames between the last step and the player's
    // thumb, not a design knob, and a per-character value would have to be
    // serialised and surfaced before anyone could set it. A jump SPENDS the
    // credit, so holding the button cannot turn the grace into a second jump.
    //
    // Refusing mid-air is a normal answer and stays silent. Only a call that
    // found nothing to act on — no character controller, or a speed of zero —
    // logs, like the rest of the write half above.
    //
    // WHY IT WRITES THE COMPONENT TOO: the character's velocity lives in Jolt,
    // but MovementSystem rebuilds it every tick as (planar.x, cc.velocity.y,
    // planar.z) — it must, or walking would erase the fall. If the jump only
    // reached Jolt, the next Movement tick would hand back the pre-jump Y and
    // the jump would vanish before it was ever stepped. That happens in the
    // editor (scripts run after the step, Movement before the next one) and in
    // the game on any frame where the fixed-step accumulator owes no step at
    // all. So both halves are written here and stay in agreement.
    bool jumpCharacter(uint32_t entityId);
    bool jumpCharacter(uint32_t entityId, float speed);

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
    // the last contact between the two bodies goes away.
    //
    // A body that is DESTROYED does not produce one — not via clear(), not via
    // removeEntity(), not via the reap in step(). That is deliberate: Jolt still
    // fires OnContactRemoved for a destroyed body's cached contacts during the
    // next Update(), and reporting those would hand game code an exit event for
    // an entity that no longer exists. "It was destroyed" is not "it stopped
    // touching me", and the code that reacts to an exit almost never survives
    // being handed a dead entity id.
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
    // The per-entity halves of initialize(), shared with addEntity() so the two
    // paths cannot drift — two copies of "how a body is built" is exactly the
    // kind of divergence this class was audited for. Each returns whether it
    // built anything.
    //
    // All three compose the entity's WORLD pose from its parent chain rather than
    // reading TransformComponent's own fields. That walk is per BUILD — scene
    // start, or one spawn — never per frame: step() moves bodies, it does not
    // rebuild them.
    bool buildBodyFor(HorizonWorld& world, uint32_t entityId);
    bool buildCharacterFor(HorizonWorld& world, uint32_t entityId);
    // The implicit landscape collider: a static height field for a terrain
    // entity that carries no RigidBodyComponent of its own.
    bool buildTerrainBodyFor(HorizonWorld& world, uint32_t entityId);
    // The teardown half, shared by clear(), removeEntity() and the reap in
    // step(). Includes dropping the entity's contact bookkeeping.
    void destroyBodyFor(uint32_t entityId);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
