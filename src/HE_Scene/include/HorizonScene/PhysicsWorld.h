#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <vector>

class HorizonWorld;
class ContentManager;
namespace HE { struct CollisionLayerConfig; }
#include <Types/Enums.h>   // HE::JointType, named by JointDesc below

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
        // Which of the sixteen collision channels the thing that was hit sits
        // in. Appended here rather than left to the caller because the only
        // other way to it is looking the entity up in the registry and reading
        // its RigidBodyComponent — a lookup to learn something the query
        // already had in its hand. Meaningless when `hit` is false, like every
        // field above it.
        uint8_t   layer    = 0;
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

    // The project's collision matrix: which of the sixteen named channels may
    // touch which. A body's channel comes from RigidBodyComponent::
    // collisionLayer, a character's from CharacterControllerComponent::
    // collisionLayer, and the implicit landscape height field is fixed to the
    // Terrain channel.
    //
    // The editor calls this when a project is opened and whenever the matrix is
    // edited; GameApplication calls it once at start from the packaged
    // ProjectConfig. WITHOUT A CALL the default-constructed config applies —
    // every channel collides with every other, which is exactly how this class
    // behaved before channels existed. That is the contract: an existing project
    // that never heard of layers simulates unchanged.
    //
    // Safe to call while the simulation runs: the matrix is copied into the
    // filter Jolt holds, so the change takes effect from the next broadphase
    // update. Contacts that already exist are resolved once more and then let
    // go, so a pair switched off separates over a step rather than in the same
    // instant — which is what anyone editing the matrix during play wants to
    // see anyway.
    void setCollisionLayers(const HE::CollisionLayerConfig& config);

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
    //
    // This is a PERMANENT removal, and it takes the joints with it: the one this
    // entity authored and the ones other entities aimed at it are destroyed
    // outright, not put back on the pending list. A surviving partner's joint
    // does not come back when this entity is added again — only rebuilding the
    // OWNER of that joint brings it back. Anything else would leave an entry
    // waiting for a body that was deliberately deleted.
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

    // "Every channel" for the layerMask parameters below — the default, so a
    // caller who does not care about channels writes nothing and sees what the
    // query always reported.
    //
    // A MASK, not a channel index: a query asks "world and enemies, not
    // triggers", which is a set. Bit i means channel i is eligible; sixteen of
    // the thirty-two bits mean anything, and the rest simply never match, so an
    // over-wide mask needs no validation. Note that this filters what the query
    // may SEE — it has nothing to do with the collision matrix, which decides
    // what the simulation resolves. A ray fired on the Player channel is not
    // restricted to what a player collides with; it sees exactly what its mask
    // names.
    static constexpr uint32_t kAllLayers = 0xFFFFFFFFu;

    // Cast a ray from `origin` along `direction` (need not be normalised) up to
    // `maxDistance` metres. Returns the closest hit or RaycastHit{hit=false}.
    //
    // `ignoreEntityId` skips that entity's body — without it a ray fired from
    // something's own position reports that something, which is never what the
    // caller meant. Triggers ARE reported, as they always were; callers that
    // care check the hit entity.
    //
    // `layerMask` narrows what the ray may see to the channels whose bit is set
    // (kAllLayers = every one, and the parameter is last so no existing caller
    // changes). The filter sits in Jolt's OBJECT-LAYER slot, ahead of the narrow
    // phase, so a ray that ignores fifteen channels also skips their triangle
    // tests instead of doing them and discarding the answer.
    RaycastHit raycast(const glm::vec3& origin,
                       const glm::vec3& direction,
                       float            maxDistance = 1000.0f,
                       uint32_t         ignoreEntityId = kNoEntity,
                       uint32_t         layerMask = kAllLayers) const;

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
    //
    // `layerMask` as on raycast: which channels the sweep may see at all.
    RaycastHit sphereCast(const glm::vec3& origin,
                          const glm::vec3& direction,
                          float            radius,
                          float            maxDistance,
                          uint32_t         ignoreEntityId = kNoEntity,
                          uint32_t         layerMask = kAllLayers) const;

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
    //
    // `layerMask` as on raycast: which channels count as being "here".
    std::vector<uint32_t> overlapSphere(const glm::vec3& center,
                                        float            radius,
                                        uint32_t         ignoreEntityId = kNoEntity,
                                        uint32_t         layerMask = kAllLayers) const;

    // ── The same two questions, asked with a shape that has an orientation ───
    // A sphere is the only shape that needs no rotation, which is why it came
    // first and why these four arrive together: a box and a capsule are only
    // useful once they can lie on their side.
    //
    // `rotationEuler` is in DEGREES and is read exactly as TransformComponent::
    // rotation is (`glm::quat(glm::radians(euler))`, one shared expression) — so
    // a box cast at (0, 45, 0) is the same box a body at (0, 45, 0) is, which is
    // the only reason this parameter is worth having. A second Euler convention
    // here would be a bug nobody could see.
    //
    // Everything else matches sphereCast/overlapSphere and is documented there:
    // a CAST skips triggers and reports where the shape's ORIGIN stopped (not
    // the contact point, so a camera does not end up inside the wall), an
    // OVERLAP reports triggers and answers with entities. `ignoreEntityId` and
    // `layerMask` mean what they mean everywhere else.
    //
    // A degenerate shape is a miss, not an assert: a half extent or a radius of
    // zero or less answers "nothing" rather than reaching Jolt, which would
    // trip an assertion in a debug build over what is really a caller's empty
    // query.
    RaycastHit boxCast(const glm::vec3& origin,
                       const glm::vec3& halfExtents,
                       const glm::vec3& rotationEuler,
                       const glm::vec3& direction,
                       float            maxDistance,
                       uint32_t         ignoreEntityId = kNoEntity,
                       uint32_t         layerMask = kAllLayers) const;

    // `height` is the FULL height including both caps, the same number
    // ColliderComponent::height carries — so a character's capsule can be swept
    // ahead of the character by handing this its own two fields. A height of at
    // most twice the radius is a sphere and is treated as one (the cylinder in
    // the middle gets a hair of length rather than being refused).
    RaycastHit capsuleCast(const glm::vec3& origin,
                           float            radius,
                           float            height,
                           const glm::vec3& rotationEuler,
                           const glm::vec3& direction,
                           float            maxDistance,
                           uint32_t         ignoreEntityId = kNoEntity,
                           uint32_t         layerMask = kAllLayers) const;

    std::vector<uint32_t> overlapBox(const glm::vec3& center,
                                     const glm::vec3& halfExtents,
                                     const glm::vec3& rotationEuler,
                                     uint32_t         ignoreEntityId = kNoEntity,
                                     uint32_t         layerMask = kAllLayers) const;

    std::vector<uint32_t> overlapCapsule(const glm::vec3& center,
                                         float            radius,
                                         float            height,
                                         const glm::vec3& rotationEuler,
                                         uint32_t         ignoreEntityId = kNoEntity,
                                         uint32_t         layerMask = kAllLayers) const;

    // Every body along the ray, not just the first — a bullet that passes
    // through two enemies, a line of sight that has to know it crossed glass
    // before it reached the player.
    //
    // SORTED, nearest first. Jolt promises no order at all, and "the first thing
    // the shot hits" is the question every caller actually asks, so the sort is
    // done here rather than left as a trap.
    //
    // ONE ENTRY PER ENTITY, the nearest one. A concave mesh reports its entry
    // and its far wall as two hits of the same body, and "the bullet passed
    // through the house twice" is not what the caller meant — the same reason
    // overlapSphere de-duplicates its sub-shape reports.
    //
    // Triggers are reported (it is a ray, and raycast reports them), and hit[0]
    // is the same hit `raycast` would have returned for the same arguments.
    std::vector<RaycastHit> raycastAll(const glm::vec3& origin,
                                       const glm::vec3& direction,
                                       float            maxDistance = 1000.0f,
                                       uint32_t         ignoreEntityId = kNoEntity,
                                       uint32_t         layerMask = kAllLayers) const;

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

    // The same push, applied somewhere OTHER than the centre of mass — so it
    // spins the body as well as moving it. This is the difference between a
    // crate sliding away from an explosion and a crate tumbling away from it,
    // and it is the pair addTorque cannot express: a torque alone spins in
    // place, a force at the centre alone never spins at all.
    //
    // `worldPosition` is a WORLD point, and it is the one place in this engine's
    // gameplay surface where a position sitting next to an entity is not that
    // entity's local space. The reason is that it is not a pose: it is where in
    // the world the push lands, and every source of one — a raycast hit point,
    // an explosion's centre, a contact — is already a world point. Converting it
    // through the entity's parent chain would turn "push the door at its handle"
    // into a push at some point that depends on what the door happens to be
    // parented to. Same rule at the HE::api::physics level, said again there.
    //
    // A point far from the body pushes it just as hard: Jolt does not care
    // whether the point is inside the shape. Passing the body's own centre makes
    // these exactly addForce/addImpulse.
    bool addForceAtPosition(uint32_t entityId, const glm::vec3& force,
                            const glm::vec3& worldPosition);
    bool addImpulseAtPosition(uint32_t entityId, const glm::vec3& impulse,
                              const glm::vec3& worldPosition);

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

    // Spin, in RADIANS PER SECOND about the world axes. Radians because it is a
    // RATE rather than a pose, and every physics number it is combined with (a
    // torque, an inertia) is in radians. A full turn a second is (0, 6.283, 0).
    //
    // That is the rule the whole surface follows, not an exception: a POSE is in
    // degrees (an entity's rotation, a shape cast's rotation, a hinge's limits)
    // and a RATE is in radians. The other rate is setJointMotor's target speed
    // for a hinge, and it says so where it lives.
    //
    // RIGID BODIES ONLY, and no character dispatch like the pair above: a
    // CharacterVirtual has no angular velocity at all — it is kept upright by
    // definition — so an entity that only has a controller reads zero and
    // refuses the write. Kinematic bodies accept it (they are driven, and a
    // driven platform may well rotate); a static one refuses and logs, like
    // every other write here.
    bool      setAngularVelocity(uint32_t entityId, const glm::vec3& angularVelocity);
    glm::vec3 getAngularVelocity(uint32_t entityId) const;

    // ── Joints ───────────────────────────────────────────────────────────────
    // A door on its frame, a link in a chain, a rope between a grapple and a
    // wall. Everything an author can set lives on JointComponent, which is the
    // ONE source of truth: the runtime calls below write it and then build from
    // it, so a joint hooked up mid-game is still there after a save and a load.
    //
    // The per-type table of which fields mean anything lives on the component;
    // it is not repeated here, because two copies of it would disagree.
    struct JointDesc
    {
        HE::JointType type = HE::JointType::Fixed;
        glm::vec3 anchorA{ 0.0f };   // LOCAL to entity A — see the component
        glm::vec3 anchorB{ 0.0f };   // LOCAL to entity B, and read by Distance alone
        glm::vec3 axis{ 0.0f, 1.0f, 0.0f };  // LOCAL to A; hinge axis / slider direction
        float     minLimit = 0.0f;   // DEGREES (hinge) or metres (slider)
        float     maxLimit = 0.0f;   // min >= max means unlimited
        // Hinge and Slider only. RADIANS per second / metres per second, and
        // the FORCE is the switch — see the component.
        float     motorTarget   = 0.0f;
        float     motorMaxForce = 0.0f;
        float     breakForce    = 0.0f;   // newtons; 0 never breaks
        bool      collideConnected = false;   // may the two bodies touch
    };

    // Join entityA to entityB. Writes entityA's JointComponent from `desc` — the
    // component is the source of truth, and a joint that only existed inside
    // Jolt would vanish the next time the scene was saved.
    //
    // IDEMPOTENT, like addEntity: an entity that already has a joint has it torn
    // down first, so this is also how a joint is changed.
    //
    // BOTH SIDES NEED A RIGID BODY. A CharacterController is not a body
    // (CharacterVirtual has none), so an entity that only has a controller
    // cannot be jointed to anything — that is refused with a log, not silently
    // ignored. Two STATIC bodies are refused for the same reason: nothing could
    // ever move, so the joint would be a lie in the outliner.
    //
    // Returns whether the joint now exists in the simulation. It can answer
    // false while leaving the component behind: a partner that has not spawned
    // yet is a legitimate order to build the joint later, and it is retried as
    // each following entity gains its body.
    bool addJoint(HorizonWorld& world, uint32_t entityA, uint32_t entityB,
                  const JointDesc& desc);

    // Undo that: the constraint goes and so does the component. Removing the
    // component too is the other half of "the component is the truth" — leaving
    // it would resurrect the joint on the next scene load.
    bool removeJoint(HorizonWorld& world, uint32_t entityA);

    // Is there a LIVE constraint on this entity? Not the same question as "does
    // it have a JointComponent": a joint whose partner has not spawned yet is
    // authored but not yet built, and this answers about the simulation.
    bool hasJoint(uint32_t entityA) const;

    // Drive the joint: the door opens, the platform rises, the wheel spins.
    // HINGE AND SLIDER ONLY — the other three have no single axis to drive
    // along, and asking is refused with a log rather than ignored.
    //
    // `targetSpeed` is RADIANS per second for a hinge and metres per second for
    // a slider (a rate, so radians — see setAngularVelocity). `maxForce` is the
    // switch: at or below zero the motor is OFF, and a target of zero with force
    // behind it is a BRAKE that holds the joint where it is.
    //
    // Writes the component first and the live constraint second, like every
    // other joint call, so a motor started mid-game survives a save, a load and
    // a rebuild of either body.
    bool setJointMotor(HorizonWorld& world, uint32_t entityA,
                       float targetSpeed, float maxForce);

    // How much force the joint carries before it lets go, in newtons; 0 never
    // breaks. Live: no rebuild, and the joint that is already over the limit
    // breaks on the next step.
    bool setJointBreakForce(HorizonWorld& world, uint32_t entityA, float breakForce);

    // May the two jointed bodies touch each other? False is the default and what
    // a chain wants — see the component. Takes effect on the next step; contacts
    // that already exist are not retro-actively removed, which matters for one
    // frame and never again.
    bool setJointCollideConnected(HorizonWorld& world, uint32_t entityA, bool collide);

    // Every joint that broke since the last call, as the entity pair it joined —
    // `entityA` is the one that owned the JointComponent. Drained by whoever
    // calls this, like the contact queues, and for the same reason: an event
    // nobody took is an event that happened once.
    //
    // Unlike the contact queues there is no dispatcher that drains this one on
    // its own — CollisionSystem has a callback to deliver a contact to and none
    // to deliver a broken joint to, so the only consumers are pollers. A session
    // whose scripts never ask would therefore grow the queue forever, which is
    // what kMaxBrokenJoints bounds: past it the OLDEST entry is dropped and a
    // throttled warning says nobody is asking. A caller that polls never reaches
    // the cap, so the contract above is exactly what it always was.
    //
    // A joint that was DESTROYED does not appear here — not through
    // removeJoint, not because one of its bodies was deleted, not through
    // clear(). Only a joint that lost to the forces on it. That is the same line
    // pollCollisionExit draws: "it was destroyed" is not "it broke", and game
    // code that plays a snapping sound has no use for the first.
    std::vector<CollisionEvent> pollJointBroken();

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

    // How many broken joints wait for a pollJointBroken() that may never come.
    // Public for the same reason kFixedDt is: a test that asserts the bound has
    // to be able to name it rather than repeat the number. "More than any frame
    // plausibly breaks" rather than a tuned value.
    static constexpr std::size_t kMaxBrokenJoints = 256;

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
    // The joint half of the same pair: one builder both the bulk path and the
    // runtime path go through, reading the entity's JointComponent and nothing
    // else. Returns whether a constraint now exists.
    bool buildJointFor(HorizonWorld& world, uint32_t entityId);

    // Destroy every joint that names this entity on EITHER side — the one its
    // own JointComponent authored, and the ones other entities aimed at it.
    // A Jolt constraint holds raw Body pointers, so a body destroyed underneath
    // one leaves the solver reading freed memory: this must run BEFORE the body
    // goes, from every path that destroys a body.
    //
    // `requeue` puts the affected owners back on the pending list, which is what
    // a REBUILD wants (addEntity tears the old body down and builds a new one,
    // and the chain the entity was part of has to come back) and what a removal
    // does not.
    void destroyJointsInvolving(uint32_t entityId, bool requeue);

    // Break every joint whose component names a breakForce it exceeded in the
    // step that just ran, and remember the pairs for pollJointBroken(). Runs
    // from step() right after Update(), while the solver's accumulated impulses
    // still describe the step they were solved for.
    void breakOverloadedJoints(HorizonWorld& world, float dt);

    // Try to build every joint that is authored but not yet in the simulation.
    // Run after each body is built, because "the partner does not exist yet" is
    // the normal state halfway through a spawn. Gives up on an entry that has
    // failed too often, so the list cannot become a leak.
    void resolvePendingJoints(HorizonWorld& world);

    // The teardown half behind removeEntityImpl(). Includes dropping the
    // entity's contact bookkeeping and every joint the body was part of.
    // `requeueJoints` is passed straight to destroyJointsInvolving — see there
    // for which of the two callers wants which.
    void destroyBodyFor(uint32_t entityId, bool requeueJoints);

    // The body of removeEntity(), with the one thing the two callers disagree
    // about made explicit. A REBUILD (addEntity, which tears the old body down
    // to put a new one up) wants the joints back; the public, permanent
    // removeEntity() does not. Both used to arrive here with `true`, which put
    // a surviving partner's joint on a list that could never resolve.
    void removeEntityImpl(uint32_t entityId, bool requeueJoints);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
