// Must come first — Jolt requires this before any other Jolt include.
#include <Jolt/Jolt.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

JPH_SUPPRESS_WARNINGS

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include <Diagnostics/Log.h>

#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/RigidBodyComponent.h"
#include "HorizonScene/Components/ColliderComponent.h"
#include "HorizonScene/Components/CharacterControllerComponent.h"

#include <glm/gtc/quaternion.hpp>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

// ─── Layer definitions ────────────────────────────────────────────────────────

namespace HELayers {
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING     = 1;
    static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}

namespace HEBPLayers {
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    static constexpr JPH::BroadPhaseLayer MOVING(1);
    static constexpr uint32_t NUM_LAYERS = 2;
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
    BPLayerInterfaceImpl() {
        m_objectToBP[HELayers::NON_MOVING] = HEBPLayers::NON_MOVING;
        m_objectToBP[HELayers::MOVING]     = HEBPLayers::MOVING;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return HEBPLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        JPH_ASSERT(layer < HELayers::NUM_LAYERS);
        return m_objectToBP[layer];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return (layer == HEBPLayers::NON_MOVING) ? "NON_MOVING" : "MOVING";
    }
#endif
private:
    JPH::BroadPhaseLayer m_objectToBP[HELayers::NUM_LAYERS];
};

class ObjectVsBPLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer obj, JPH::BroadPhaseLayer bp) const override {
        switch (obj) {
        case HELayers::NON_MOVING: return bp == HEBPLayers::MOVING;
        case HELayers::MOVING:     return true;
        default: JPH_ASSERT(false); return false;
        }
    }
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        switch (a) {
        case HELayers::NON_MOVING: return b == HELayers::MOVING;
        case HELayers::MOVING:     return true;
        default: JPH_ASSERT(false); return false;
        }
    }
};

// ─── Process-global Jolt init (run once, never torn down) ─────────────────────
// RegisterTypes / Factory are global state inside Jolt; re-registering after
// UnregisterTypes corrupts internal maps. We initialise once per process and
// intentionally skip the shutdown (the small factory object is reclaimed on exit).
static void joltEnsureInit()
{
    static std::once_flag s_flag;
    std::call_once(s_flag, []() {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        HE_LOG_INFO(Physics, "%s", "Jolt physics initialised (process-global types registered)");
    });
}

// ─── Contact listener — buffers collision enter/exit events thread-safely ────
class HEContactListener : public JPH::ContactListener
{
public:
    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                        const JPH::ContactManifold&, JPH::ContactSettings&) override
    {
        PhysicsWorld::CollisionEvent ev;
        ev.entityA = static_cast<uint32_t>(b1.GetUserData());
        ev.entityB = static_cast<uint32_t>(b2.GetUserData());

        // A contact involving a SENSOR is an overlap, everything else a blocking
        // hit. The split lives here because it is the only place that can see
        // the bodies — and doing it here means a graph never has to ask which
        // kind of contact it was handed.
        const bool overlap = b1.IsSensor() || b2.IsSensor();

        std::lock_guard<std::mutex> lock(m_mutex);
        // Jolt reports contacts per *sub shape* pair, so one body pair can produce
        // several callbacks (compound/mesh shapes). Gameplay only cares about the
        // body pair, hence the ref count: enter fires on the first sub-shape
        // contact, exit on the last one.
        ActiveContact& contact = m_active[bodyPairKey(b1.GetID(), b2.GetID())];
        if (contact.refCount++ == 0)
        {
            // Cache the entity ids AND the kind *here*: OnContactRemoved is not
            // allowed to look at the bodies at all, so this is the only place we
            // can resolve either.
            contact.event   = ev;
            contact.overlap = overlap;
            (overlap ? m_enteredOverlap : m_entered).push_back(ev);
        }
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        // Called from the physics job with all bodies locked — and the bodies may
        // already have been destroyed. We therefore never touch them here and
        // resolve the entity ids from the cache filled in OnContactAdded.
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_active.find(bodyPairKey(pair.GetBody1ID(), pair.GetBody2ID()));
        if (it == m_active.end())
            return;   // contact belongs to a torn-down scene (see reset()) — drop it
        if (--it->second.refCount == 0)
        {
            (it->second.overlap ? m_exitedOverlap : m_exited).push_back(it->second.event);
            m_active.erase(it);
        }
    }

    std::vector<PhysicsWorld::CollisionEvent> pollEntered()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<PhysicsWorld::CollisionEvent> result;
        result.swap(m_entered);
        return result;
    }

    std::vector<PhysicsWorld::CollisionEvent> pollOverlapEntered()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<PhysicsWorld::CollisionEvent> result;
        result.swap(m_enteredOverlap);
        return result;
    }

    std::vector<PhysicsWorld::CollisionEvent> pollOverlapExited()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<PhysicsWorld::CollisionEvent> result;
        result.swap(m_exitedOverlap);
        return result;
    }

    std::vector<PhysicsWorld::CollisionEvent> pollExited()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<PhysicsWorld::CollisionEvent> result;
        result.swap(m_exited);
        return result;
    }

    // Forget every buffered event and every tracked contact. PhysicsWorld::clear()
    // calls this after destroying the bodies: Jolt still emits OnContactRemoved for
    // their cached contacts during the next Update(), and without this the listener
    // would report exit events for entities of the previous scene.
    void reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active.clear();
        m_entered.clear();
        m_exited.clear();
        m_enteredOverlap.clear();
        m_exitedOverlap.clear();
    }

private:
    struct ActiveContact
    {
        uint32_t                     refCount = 0;
        PhysicsWorld::CollisionEvent event;
        // Which queue this contact's EXIT belongs in. Decided on the add side,
        // because OnContactRemoved may not touch the bodies to ask again.
        bool                         overlap = false;
    };

    // Jolt already hands out body pairs sorted by BodyID, but sort defensively so
    // the add- and remove-side keys can never disagree. Index+sequence number is
    // used (not the raw index) so a recycled body slot yields a different key.
    static uint64_t bodyPairKey(const JPH::BodyID& a, const JPH::BodyID& b)
    {
        uint32_t lo = a.GetIndexAndSequenceNumber();
        uint32_t hi = b.GetIndexAndSequenceNumber();
        if (lo > hi)
            std::swap(lo, hi);
        return (static_cast<uint64_t>(lo) << 32) | static_cast<uint64_t>(hi);
    }

    std::mutex m_mutex;
    std::unordered_map<uint64_t, ActiveContact> m_active;
    std::vector<PhysicsWorld::CollisionEvent> m_entered;
    std::vector<PhysicsWorld::CollisionEvent> m_exited;
    std::vector<PhysicsWorld::CollisionEvent> m_enteredOverlap;
    std::vector<PhysicsWorld::CollisionEvent> m_exitedOverlap;
};

// ─── Impl ─────────────────────────────────────────────────────────────────────
struct PhysicsWorld::Impl
{
    BPLayerInterfaceImpl     bpLayerInterface;
    ObjectVsBPLayerFilterImpl ovbpFilter;
    ObjectLayerPairFilterImpl ooFilter;

    // Both must outlive every Update() call — keep as members.
    JPH::TempAllocatorImpl       tempAllocator{ 10u * 1024u * 1024u };
    JPH::JobSystemSingleThreaded jobSystem;
    JPH::PhysicsSystem           physicsSystem;
    HEContactListener            contactListener;

    // Entity id (cast to uint32_t) → Jolt body id
    std::unordered_map<uint32_t, JPH::BodyID> entityToBody;

    // Entity id → CharacterVirtual (for CharacterControllerComponent entities)
    std::unordered_map<uint32_t, std::unique_ptr<JPH::CharacterVirtual>> entityToCharacter;

    bool initialized = false;

    // Hard limits handed to Jolt below. Exceeding them makes CreateAndAddBody
    // return an invalid id, which used to be swallowed silently — the scene then
    // simply had objects that never fell. They are logged instead.
    static constexpr uint32_t kMaxBodies = 1024;

    Impl()
    {
        jobSystem.Init(JPH::cMaxPhysicsJobs);
        physicsSystem.Init(
            kMaxBodies,   // max bodies
            0,      // num body mutexes (0 = auto)
            1024,   // max body pairs
            1024,   // max contact constraints
            bpLayerInterface,
            ovbpFilter,
            ooFilter
        );
        physicsSystem.SetContactListener(&contactListener);
    }
};

// ─── PhysicsWorld ─────────────────────────────────────────────────────────────
PhysicsWorld::PhysicsWorld()
{
    joltEnsureInit();
    m_impl = std::make_unique<Impl>();
}

PhysicsWorld::~PhysicsWorld()
{
    clear();
}

void PhysicsWorld::initialize(HorizonWorld& world)
{
    clear();

    auto& reg           = world.registry();
    auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();

    for (auto [entity, transform, rb] :
         reg.view<TransformComponent, RigidBodyComponent>().each())
    {
        // Build shape: prefer ColliderComponent if present, fall back to transform.scale box.
        auto* col = reg.try_get<ColliderComponent>(entity);
        JPH::ShapeSettings::ShapeResult shapeResult;
        if (col)
        {
            switch (col->shape)
            {
            case ColliderShape::Sphere:
                shapeResult = JPH::SphereShapeSettings(
                    std::max(0.01f, col->radius)
                ).Create();
                break;
            case ColliderShape::Capsule: {
                float halfCyl = std::max(0.0f, col->height * 0.5f - col->radius);
                shapeResult = JPH::CapsuleShapeSettings(
                    halfCyl, std::max(0.01f, col->radius)
                ).Create();
                break;
            }
            default: // Box
                shapeResult = JPH::BoxShapeSettings(
                    JPH::Vec3(std::max(0.01f, col->halfExtents.x),
                              std::max(0.01f, col->halfExtents.y),
                              std::max(0.01f, col->halfExtents.z))
                ).Create();
                break;
            }
        }
        else
        {
            glm::vec3 halfEx = glm::max(transform.scale * 0.5f, glm::vec3(0.01f));
            shapeResult = JPH::BoxShapeSettings(
                JPH::Vec3(halfEx.x, halfEx.y, halfEx.z)
            ).Create();
        }
        if (shapeResult.HasError())
        {
            HE_LOG_ERROR(Physics, "Entity %u: rigid-body collider shape could not be built (%s) "
                                  "— entity has no physics body",
                         static_cast<uint32_t>(entity), shapeResult.GetError().c_str());
            continue;
        }

        // Euler angles (degrees) → quaternion, matching the engine's convention
        glm::quat gq = glm::quat(glm::radians(transform.rotation));
        JPH::Quat jq { gq.x, gq.y, gq.z, gq.w };

        JPH::RVec3 pos(transform.position.x, transform.position.y, transform.position.z);

        JPH::EMotionType motionType;
        JPH::ObjectLayer layer;
        switch (rb.type)
        {
        case RigidBodyType::Dynamic:
            motionType = JPH::EMotionType::Dynamic;
            layer      = HELayers::MOVING;
            break;
        case RigidBodyType::Kinematic:
            motionType = JPH::EMotionType::Kinematic;
            layer      = HELayers::MOVING;
            break;
        default: // Static
            motionType = JPH::EMotionType::Static;
            layer      = HELayers::NON_MOVING;
            break;
        }

        JPH::BodyCreationSettings bcs(shapeResult.Get(), pos, jq, motionType, layer);
        if (rb.type == RigidBodyType::Dynamic)
        {
            bcs.mMassPropertiesOverride.mMass = rb.mass;
            bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        }
        bcs.mFriction    = rb.friction;
        bcs.mRestitution = rb.restitution;
        // ColliderComponent::isTrigger was authored, serialised and drawn in its
        // own debug colour, but never reached Jolt — so a trigger volume blocked
        // like a wall and produced no overlap events at all. A sensor passes
        // bodies through and still reports contacts, which is what the
        // OnBeginOverlap / OnEndOverlap pair is built from.
        bcs.mIsSensor = col && col->isTrigger;

        JPH::EActivation activation = (motionType == JPH::EMotionType::Static)
            ? JPH::EActivation::DontActivate
            : JPH::EActivation::Activate;

        // Store entity ID in body user data for reverse lookup during raycasts.
        bcs.mUserData = static_cast<uint64_t>(static_cast<uint32_t>(entity));

        JPH::BodyID bodyId = bodyInterface.CreateAndAddBody(bcs, activation);
        if (!bodyId.IsInvalid())
        {
            m_impl->entityToBody[static_cast<uint32_t>(entity)] = bodyId;
        }
        else
        {
            HE_LOG_ERROR(Physics, "Entity %u: body creation failed — the %u-body limit is "
                                  "most likely exhausted (%zu created so far)",
                         static_cast<uint32_t>(entity), Impl::kMaxBodies,
                         m_impl->entityToBody.size());
        }
    }

    // ── CharacterController entities ──────────────────────────────────────────
    for (auto [entity, transform, cc] :
         reg.view<TransformComponent, CharacterControllerComponent>().each())
    {
        // An entity carrying BOTH a CharacterController and a RigidBody used to
        // be skipped here — the comment said "skip as body", but this is the
        // CHARACTER loop, so what it actually skipped was the character.
        //
        // That combination is not exotic: EntityHost::defaultComponents gives it
        // to every PlayerCharacter, because the kinematic body is the collision
        // proxy other bodies see and the character is what moves. Skipping the
        // character left those entities as a bare kinematic body — unable to
        // walk, never grounded, with movement.* reading zeros forever. A default
        // player could not move at all.
        //
        // Both are created now. The two are kept from fighting elsewhere: the
        // body write-back skips character entities (the character owns the
        // transform), the character ignores its own body while stepping, and the
        // body is dragged along after (see step()).

        // Use ColliderComponent if present (Capsule/Box/Sphere), else default capsule.
        JPH::ShapeSettings::ShapeResult shapeResult;
        auto* col = reg.try_get<ColliderComponent>(entity);
        if (col && col->shape == ColliderShape::Capsule)
        {
            float halfCyl = std::max(0.0f, col->height * 0.5f - col->radius);
            shapeResult = JPH::CapsuleShapeSettings(halfCyl, std::max(0.01f, col->radius)).Create();
        }
        else if (col && col->shape == ColliderShape::Sphere)
        {
            shapeResult = JPH::SphereShapeSettings(std::max(0.01f, col->radius)).Create();
        }
        else
        {
            // Default: capsule height=2.0, radius=0.3
            shapeResult = JPH::CapsuleShapeSettings(0.7f, 0.3f).Create();
        }
        if (shapeResult.HasError())
        {
            HE_LOG_ERROR(Physics, "Entity %u: character-controller shape could not be built (%s) "
                                  "— the character will not move",
                         static_cast<uint32_t>(entity), shapeResult.GetError().c_str());
            continue;
        }

        JPH::CharacterVirtualSettings cvs;
        cvs.mMass                  = cc.mass;
        cvs.mCharacterPadding      = cc.skinWidth;
        cvs.mShape                 = shapeResult.Get();
        cvs.mUp                    = JPH::Vec3::sAxisY();
        cvs.mMaxSlopeAngle         = JPH::DegreesToRadians(cc.slopeLimit);

        JPH::RVec3 pos(transform.position.x, transform.position.y, transform.position.z);
        glm::quat gq = glm::quat(glm::radians(transform.rotation));
        JPH::Quat jq { gq.x, gq.y, gq.z, gq.w };

        auto character = std::make_unique<JPH::CharacterVirtual>(
            &cvs, pos, jq,
            static_cast<uint64_t>(static_cast<uint32_t>(entity)),
            &m_impl->physicsSystem
        );

        m_impl->entityToCharacter[static_cast<uint32_t>(entity)] = std::move(character);
    }

    m_impl->physicsSystem.OptimizeBroadPhase();
    m_impl->initialized = true;

    HE_LOG_INFO(Physics, "Physics world initialised: %zu rigid body/-ies, %zu character controller(s)",
                m_impl->entityToBody.size(), m_impl->entityToCharacter.size());
    if (m_impl->entityToBody.size() > Impl::kMaxBodies * 9 / 10)
        HE_LOG_WARN(Physics, "Body count %zu is close to the hard limit of %u",
                    m_impl->entityToBody.size(), Impl::kMaxBodies);
}

void PhysicsWorld::step(HorizonWorld& world, float dt)
{
    if (!m_impl->initialized || dt <= 0.0f)
        return;

    // A physics step that overruns this badly stalls the whole frame; throttled so
    // a permanently overloaded scene reports once a second, not 60 times.
    HE_LOG_SLOW_SCOPE(Physics, 8.0, "PhysicsWorld::step");

    m_impl->physicsSystem.Update(dt, 1,
        &m_impl->tempAllocator, &m_impl->jobSystem);

    auto& reg           = world.registry();
    auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();

    for (auto& [entityId, bodyId] : m_impl->entityToBody)
    {
        // Only write back non-static bodies
        if (bodyInterface.GetMotionType(bodyId) == JPH::EMotionType::Static)
            continue;

        Entity entity = static_cast<Entity>(entityId);
        if (!reg.valid(entity))
            continue;

        // An entity that also has a character controller belongs to the character
        // loop below — this one must not touch its transform at all.
        //
        // Both components on one entity is the NORMAL case, not an edge case:
        // EntityHost::defaultComponents gives every PlayerCharacter a character
        // controller AND a kinematic rigid body (the body is the collision proxy
        // other bodies see; the controller is what moves). Nothing ever pushes the
        // transform back INTO that body, so it still sits in its spawn pose — and
        // writing that pose out here every step is how a character ends up unable
        // to turn. Position survived only by accident, because the character loop
        // runs after this one and overwrites it; rotation is written nowhere else,
        // so it was silently pinned to the spawn value. That is why game code could
        // not rotate a player: scripts run BEFORE the step, so their write was gone
        // the same frame.
        if (reg.all_of<CharacterControllerComponent>(entity))
            continue;

        auto* transform = reg.try_get<TransformComponent>(entity);
        if (!transform)
            continue;

        JPH::RVec3 pos = bodyInterface.GetCenterOfMassPosition(bodyId);
        JPH::Quat  rot = bodyInterface.GetRotation(bodyId);

        // A body that goes non-finite (degenerate shape, absurd mass, a huge
        // impulse from a script) writes NaN into the transform, and from there
        // into the render matrices — where it shows up as "the mesh vanished"
        // with nothing in the log. Catch it at the source.
        if (!std::isfinite(static_cast<float>(pos.GetX())) ||
            !std::isfinite(static_cast<float>(pos.GetY())) ||
            !std::isfinite(static_cast<float>(pos.GetZ())))
        {
            HE_LOG_THROTTLE(Physics, Error, 5.0,
                            "Entity %u: physics body position is not finite — "
                            "transform write-back skipped", entityId);
            continue;
        }

        transform->position = {
            static_cast<float>(pos.GetX()),
            static_cast<float>(pos.GetY()),
            static_cast<float>(pos.GetZ())
        };

        // Jolt quat → glm quat → Euler degrees (inverse of the init path)
        glm::quat gq(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
        transform->rotation = glm::degrees(glm::eulerAngles(gq));
        transform->dirty    = true;
    }

    // ── Character controller update ────────────────────────────────────────────
    JPH::DefaultBroadPhaseLayerFilter bpFilter(m_impl->ovbpFilter, HELayers::MOVING);
    JPH::DefaultObjectLayerFilter     olFilter(m_impl->ooFilter,   HELayers::MOVING);
    JPH::BodyFilter                   bodyFilter;
    JPH::ShapeFilter                  shapeFilter;
    JPH::CharacterVirtual::ExtendedUpdateSettings euSettings;

    for (auto& [entityId, character] : m_impl->entityToCharacter)
    {
        Entity entity = static_cast<Entity>(entityId);
        if (!reg.valid(entity))
            continue;

        auto* cc        = reg.try_get<CharacterControllerComponent>(entity);
        auto* transform = reg.try_get<TransformComponent>(entity);
        if (!cc || !transform)
            continue;

        // Use Jolt character's current velocity so setCharacterVelocity() takes effect.
        // cc->velocity is an output field — game code drives velocity via setCharacterVelocity.
        float grav = cc->gravity;
        JPH::Vec3 vel = character->GetLinearVelocity();
        if (!character->IsSupported())
            vel.SetY(vel.GetY() - grav * dt);
        character->SetLinearVelocity(vel);

        JPH::Vec3 gravity(0.0f, -grav, 0.0f);
        euSettings.mWalkStairsStepUp = JPH::Vec3(0, cc->stepHeight, 0);

        // Ignore its OWN collision proxy. The kinematic body now follows the
        // character (see the MoveKinematic below), which means it sits exactly
        // where the character is — and a character that collides with itself
        // cannot move at all. Before the proxy followed, this only bit at the
        // spawn point, which is why it went unnoticed.
        const auto bodyIt = m_impl->entityToBody.find(entityId);
        const JPH::IgnoreSingleBodyFilter selfFilter(
            bodyIt != m_impl->entityToBody.end() ? bodyIt->second : JPH::BodyID());

        character->ExtendedUpdate(dt, gravity, euSettings,
            bpFilter, olFilter, selfFilter, shapeFilter,
            m_impl->tempAllocator);

        // Sync position back to transform
        JPH::RVec3 pos = character->GetPosition();
        transform->position = {
            static_cast<float>(pos.GetX()),
            static_cast<float>(pos.GetY()),
            static_cast<float>(pos.GetZ())
        };
        transform->dirty = true;

        // Sync velocity and ground state back to component
        JPH::Vec3 newVel = character->GetLinearVelocity();
        cc->velocity   = { newVel.GetX(), newVel.GetY(), newVel.GetZ() };
        cc->isGrounded = (character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround);

        // ── Drag the collision proxy along ───────────────────────────────────
        // A character usually carries BOTH a CharacterVirtual (what moves) and a
        // kinematic rigid body (what everything else collides with) — that is
        // what EntityHost::defaultComponents gives every PlayerCharacter. But
        // nothing ever moved that body: it was placed at initialize() and stayed
        // there. So the character walked away and left a ghost of itself at its
        // spawn point, which other bodies bumped into and which a camera boom
        // saw as a wall.
        //
        // MoveKinematic rather than SetPosition: it moves the body over the step
        // so contacts and velocities come out right, instead of teleporting it
        // through whatever is in between.
        if (bodyIt != m_impl->entityToBody.end())
        {
            const glm::quat q = glm::quat(glm::radians(transform->rotation));
            bodyInterface.MoveKinematic(bodyIt->second,
                JPH::RVec3(transform->position.x, transform->position.y, transform->position.z),
                JPH::Quat(q.x, q.y, q.z, q.w), dt);
        }
    }
}

namespace {

// Skips one entity's body, and optionally every sensor.
//
// Both decisions are made in ShouldCollideLocked rather than the BodyID
// overload: the entity id lives in the body's user data and IsSensor is a body
// member, so this is the one callback that can see either. The extra lock is
// paid only for bodies the broad phase already accepted.
class HEQueryFilter final : public JPH::BodyFilter
{
public:
    HEQueryFilter(uint32_t ignoreEntityId, bool skipSensors)
        : m_ignore(ignoreEntityId), m_skipSensors(skipSensors) {}

    bool ShouldCollideLocked(const JPH::Body& inBody) const override
    {
        if (m_skipSensors && inBody.IsSensor())                       return false;
        if (static_cast<uint32_t>(inBody.GetUserData()) == m_ignore)   return false;
        return true;
    }

private:
    uint32_t m_ignore;
    bool     m_skipSensors;
};

} // namespace

PhysicsWorld::RaycastHit PhysicsWorld::raycast(
    const glm::vec3& origin,
    const glm::vec3& direction,
    float            maxDistance,
    uint32_t         ignoreEntityId) const
{
    RaycastHit result;
    if (!m_impl || !m_impl->initialized || maxDistance <= 0.0f)
        return result;

    // Normalise direction; bail on zero-length.
    float len = std::sqrt(direction.x * direction.x +
                          direction.y * direction.y +
                          direction.z * direction.z);
    if (len < 1e-6f)
        return result;
    glm::vec3 dir = direction / len;

    JPH::RRayCast ray{
        JPH::RVec3(origin.x, origin.y, origin.z),
        JPH::Vec3(dir.x, dir.y, dir.z) * maxDistance
    };
    // Sensors stay visible to raycast — that is what it has always reported, and
    // a script asking "what is in front of me" may well mean a trigger.
    const HEQueryFilter bodyFilter(ignoreEntityId, /*skipSensors=*/false);

    JPH::RayCastResult hit;
    if (!m_impl->physicsSystem.GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, bodyFilter))
        return result;

    result.hit      = true;
    result.distance = hit.mFraction * maxDistance;

    // Hit position along the ray
    JPH::RVec3 hitPos = ray.GetPointOnRay(hit.mFraction);
    result.point = {
        static_cast<float>(hitPos.GetX()),
        static_cast<float>(hitPos.GetY()),
        static_cast<float>(hitPos.GetZ())
    };

    // Surface normal via body lock
    {
        JPH::BodyLockRead lock(m_impl->physicsSystem.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            JPH::Vec3 n = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPos);
            result.normal = { n.GetX(), n.GetY(), n.GetZ() };
            result.entityId = static_cast<uint32_t>(body.GetUserData());
        }
    }

    return result;
}

PhysicsWorld::RaycastHit PhysicsWorld::sphereCast(
    const glm::vec3& origin,
    const glm::vec3& direction,
    float            radius,
    float            maxDistance,
    uint32_t         ignoreEntityId) const
{
    RaycastHit result;
    if (!m_impl || !m_impl->initialized || maxDistance <= 0.0f || radius <= 0.0f)
        return result;

    const float len = std::sqrt(direction.x * direction.x +
                                direction.y * direction.y +
                                direction.z * direction.z);
    if (len < 1e-6f)
        return result;
    const glm::vec3 dir         = direction / len;
    const glm::vec3 displacement = dir * maxDistance;

    JPH::Ref<JPH::Shape> sphere = new JPH::SphereShape(radius);

    // The cast is expressed relative to inBaseOffset (the origin here), which is
    // what keeps the numbers small and precise far from the world origin.
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        sphere,
        JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sTranslation(JPH::RVec3(origin.x, origin.y, origin.z)),
        JPH::Vec3(displacement.x, displacement.y, displacement.z));

    JPH::ShapeCastSettings settings;
    // A camera boom asks "how far can I go", so a surface it is already touching
    // must not read as a hit at fraction 0 for the back face it is leaving.
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const HEQueryFilter bodyFilter(ignoreEntityId, /*skipSensors=*/true);

    m_impl->physicsSystem.GetNarrowPhaseQuery().CastShape(
        cast, settings, JPH::RVec3(origin.x, origin.y, origin.z), collector,
        {}, {}, bodyFilter);

    if (!collector.HadHit())
        return result;

    // Distance comes from the FRACTION, not from the contact point: the fraction
    // is where the sphere's CENTRE stopped, which already sits one radius clear
    // of the surface. Using the contact point would put the camera in the wall.
    const float fraction = std::clamp(collector.mHit.mFraction, 0.0f, 1.0f);

    result.hit      = true;
    result.distance = fraction * maxDistance;
    result.point    = origin + dir * result.distance;
    result.normal   = { collector.mHit.mPenetrationAxis.GetX(),
                        collector.mHit.mPenetrationAxis.GetY(),
                        collector.mHit.mPenetrationAxis.GetZ() };
    if (const float n = glm::length(result.normal); n > 1e-6f)
        result.normal = -result.normal / n;   // penetration axis points INTO the hit surface

    {
        JPH::BodyLockRead lock(m_impl->physicsSystem.GetBodyLockInterface(), collector.mHit.mBodyID2);
        if (lock.Succeeded())
            result.entityId = static_cast<uint32_t>(lock.GetBody().GetUserData());
    }

    return result;
}

void PhysicsWorld::setCharacterVelocity(uint32_t entityId, const glm::vec3& velocity)
{
    if (!m_impl) return;
    auto it = m_impl->entityToCharacter.find(entityId);
    if (it != m_impl->entityToCharacter.end())
        it->second->SetLinearVelocity(JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

bool PhysicsWorld::isCharacterGrounded(uint32_t entityId) const
{
    if (!m_impl) return false;
    auto it = m_impl->entityToCharacter.find(entityId);
    if (it == m_impl->entityToCharacter.end()) return false;
    return it->second->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

std::vector<PhysicsWorld::CollisionEvent> PhysicsWorld::pollCollisionEnter()
{
    if (!m_impl) return {};
    return m_impl->contactListener.pollEntered();
}

std::vector<PhysicsWorld::CollisionEvent> PhysicsWorld::pollCollisionExit()
{
    if (!m_impl) return {};
    return m_impl->contactListener.pollExited();
}

std::vector<PhysicsWorld::CollisionEvent> PhysicsWorld::pollOverlapEnter()
{
    if (!m_impl) return {};
    return m_impl->contactListener.pollOverlapEntered();
}

std::vector<PhysicsWorld::CollisionEvent> PhysicsWorld::pollOverlapExit()
{
    if (!m_impl) return {};
    return m_impl->contactListener.pollOverlapExited();
}

void PhysicsWorld::clear()
{
    if (!m_impl)
        return;

    if (!m_impl->entityToBody.empty() || !m_impl->entityToCharacter.empty())
        HE_LOG_DEBUG(Physics, "Clearing physics world: %zu body/-ies, %zu character(s)",
                     m_impl->entityToBody.size(), m_impl->entityToCharacter.size());

    auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
    for (auto& [entityId, bodyId] : m_impl->entityToBody)
    {
        bodyInterface.RemoveBody(bodyId);
        bodyInterface.DestroyBody(bodyId);
    }
    m_impl->entityToBody.clear();
    m_impl->entityToCharacter.clear();
    // After the bodies are gone: drop the contact bookkeeping, otherwise the
    // OnContactRemoved callbacks Jolt fires for them would emit exit events
    // referring to entities that no longer exist.
    m_impl->contactListener.reset();
    m_impl->initialized = false;
}
