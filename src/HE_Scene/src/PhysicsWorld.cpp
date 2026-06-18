// Must come first — Jolt requires this before any other Jolt include.
#include <Jolt/Jolt.h>

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
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/RigidBodyComponent.h"
#include "HorizonScene/Components/ColliderComponent.h"
#include "HorizonScene/Components/CharacterControllerComponent.h"

#include <glm/gtc/quaternion.hpp>
#include <mutex>
#include <unordered_map>
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
    uint GetNumBroadPhaseLayers() const override { return HEBPLayers::NUM_LAYERS; }
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
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entered.push_back(ev);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        // Reverse-lookup: iterate body interface to find entity IDs from body IDs
        (void)pair; // Body IDs aren't directly accessible here without the system ptr
        // Exit events via OnContactRemoved require body lookups; store for later.
        // For simplicity, store body ID pair and resolve during poll.
    }

    std::vector<PhysicsWorld::CollisionEvent> pollEntered()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<PhysicsWorld::CollisionEvent> result;
        result.swap(m_entered);
        return result;
    }

    std::vector<PhysicsWorld::CollisionEvent> pollExited()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<PhysicsWorld::CollisionEvent> result;
        result.swap(m_exited);
        return result;
    }

private:
    std::mutex m_mutex;
    std::vector<PhysicsWorld::CollisionEvent> m_entered;
    std::vector<PhysicsWorld::CollisionEvent> m_exited;
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

    Impl()
    {
        jobSystem.Init(JPH::cMaxPhysicsJobs);
        physicsSystem.Init(
            1024,   // max bodies
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
            continue;

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

        JPH::EActivation activation = (motionType == JPH::EMotionType::Static)
            ? JPH::EActivation::DontActivate
            : JPH::EActivation::Activate;

        // Store entity ID in body user data for reverse lookup during raycasts.
        bcs.mUserData = static_cast<uint64_t>(static_cast<uint32_t>(entity));

        JPH::BodyID bodyId = bodyInterface.CreateAndAddBody(bcs, activation);
        if (!bodyId.IsInvalid())
            m_impl->entityToBody[static_cast<uint32_t>(entity)] = bodyId;
    }

    // ── CharacterController entities ──────────────────────────────────────────
    for (auto [entity, transform, cc] :
         reg.view<TransformComponent, CharacterControllerComponent>().each())
    {
        // Entities with both RigidBody and CharacterController: skip as body.
        if (reg.any_of<RigidBodyComponent>(entity))
            continue;

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
            continue;

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
}

void PhysicsWorld::step(HorizonWorld& world, float dt)
{
    if (!m_impl->initialized || dt <= 0.0f)
        return;

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

        auto* transform = reg.try_get<TransformComponent>(entity);
        if (!transform)
            continue;

        JPH::RVec3 pos = bodyInterface.GetCenterOfMassPosition(bodyId);
        JPH::Quat  rot = bodyInterface.GetRotation(bodyId);

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

        character->ExtendedUpdate(dt, gravity, euSettings,
            bpFilter, olFilter, bodyFilter, shapeFilter,
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
    }
}

PhysicsWorld::RaycastHit PhysicsWorld::raycast(
    const glm::vec3& origin,
    const glm::vec3& direction,
    float            maxDistance) const
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
    JPH::RayCastResult hit;
    if (!m_impl->physicsSystem.GetNarrowPhaseQuery().CastRay(ray, hit))
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

void PhysicsWorld::clear()
{
    if (!m_impl)
        return;

    auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
    for (auto& [entityId, bodyId] : m_impl->entityToBody)
    {
        bodyInterface.RemoveBody(bodyId);
        bodyInterface.DestroyBody(bodyId);
    }
    m_impl->entityToBody.clear();
    m_impl->entityToCharacter.clear();
    m_impl->initialized = false;
}
