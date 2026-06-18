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

#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/RigidBodyComponent.h"
#include "HorizonScene/Components/ColliderComponent.h"

#include <glm/gtc/quaternion.hpp>
#include <mutex>
#include <unordered_map>

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

    // Entity id (cast to uint32_t) → Jolt body id
    std::unordered_map<uint32_t, JPH::BodyID> entityToBody;

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
    m_impl->initialized = false;
}
