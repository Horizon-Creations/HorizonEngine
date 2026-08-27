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
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include <Diagnostics/Log.h>
#include <ContentManager/ContentManager.h>

#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/TerrainMeshGenerator.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/RigidBodyComponent.h"
#include "HorizonScene/Components/ColliderComponent.h"
#include "HorizonScene/Components/CharacterControllerComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/LODComponent.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <glm/gtc/quaternion.hpp>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
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

    // reset() for ONE entity — what removeEntity() needs and what clear() did to
    // the whole world. Without it, destroying a single body leaves its cached
    // contacts in m_active, Jolt fires OnContactRemoved for them during the next
    // Update(), and the listener happily reports an exit event naming an entity
    // that has already been destroyed. The queued events go too: an enter that
    // nobody drained yet is about a body that no longer exists.
    //
    // A linear scan is enough. m_active holds ACTIVE CONTACTS, not bodies, and
    // a removal is a gameplay-rate event — a reverse index would be bookkeeping
    // maintained every frame to speed up something that happens on a kill.
    void purgeEntity(uint32_t entityId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_active.begin(); it != m_active.end(); )
        {
            if (it->second.event.entityA == entityId || it->second.event.entityB == entityId)
                it = m_active.erase(it);
            else
                ++it;
        }
        const auto drop = [entityId](std::vector<PhysicsWorld::CollisionEvent>& queue) {
            queue.erase(std::remove_if(queue.begin(), queue.end(),
                                       [entityId](const PhysicsWorld::CollisionEvent& ev) {
                                           return ev.entityA == entityId || ev.entityB == entityId;
                                       }),
                        queue.end());
        };
        drop(m_entered);
        drop(m_exited);
        drop(m_enteredOverlap);
        drop(m_exitedOverlap);
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

    // Where ColliderShape::Mesh / ::ConvexHull get their triangles. Nullable —
    // see PhysicsWorld::setContentManager.
    ContentManager* content = nullptr;

    // The world this simulation belongs to, remembered from whichever call last
    // named it (initialize/step/addEntity). setPosition() needs it to keep the
    // TransformComponent in step with the teleport, and it takes no world of its
    // own: a teleport is written from game code that already knows the entity id
    // and nothing else. Never dereferenced without a validity check.
    HorizonWorld* world = nullptr;

    bool initialized = false;

    // step() iterates THESE, not the maps themselves. Rebuilt each step from the
    // map keys so that adding or removing a body from game code driven by the
    // step (a spawn on landing, a pickup that deletes itself) cannot invalidate
    // the iterator underneath it — that would be a rehash mid-loop, i.e. the
    // kind of crash that only shows up in a shipped build. Members rather than
    // locals so the per-frame allocation happens once.
    std::vector<uint32_t> stepBodies;
    std::vector<uint32_t> stepCharacters;

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

// ─── World space ──────────────────────────────────────────────────────────────
namespace {

// Jolt knows exactly one space, the world's. TransformComponent stores a LOCAL
// pose, and the world pose is what falls out of walking the parent chain — so
// every pose crossing this boundary has to be converted, in both directions.
// Skipping the conversion is not a rounding error, it is two different worlds:
// an additively streamed zone hangs its whole content under the loaded scene's
// root, so its meshes stand at the zone position while its colliders sit at the
// authored origin, and NavigationSystem (which bakes from world matrices) walks
// a level the player collides with somewhere else.
//
// The walk goes through HE::TransformHierarchy rather than TransformComponent::
// worldMatrix, deliberately: worldMatrix is only as fresh as the last
// propagateTransforms(), and a body is built the moment an entity is SPAWNED —
// before anything has propagated. Reading it there hands Jolt the identity.

// A world matrix pulled apart into the three things Jolt is built from.
struct WorldPose
{
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 scale{ 1.0f };
};

WorldPose decomposeWorld(const glm::mat4& m)
{
    WorldPose out;
    out.position = glm::vec3(m[3]);

    glm::vec3 axis[3] = { glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2]) };
    const float len[3] = { glm::length(axis[0]), glm::length(axis[1]), glm::length(axis[2]) };
    out.scale = glm::vec3(len[0], len[1], len[2]);

    // A zero scale is a real input, not a pathological one — the inspector lets
    // a user type 0 and every shape builder below already clamps against it.
    // Dividing by it would put a NaN in the quaternion and from there into the
    // body, where it surfaces as "the object disappeared".
    for (int i = 0; i < 3; ++i)
        axis[i] /= std::max(len[i], 1.0e-6f);

    // Column lengths cannot see a MIRROR: an odd number of negative scale axes
    // still leaves three positive lengths and a left-handed basis, which
    // quat_cast reads as a rotation that does not exist. The determinant is what
    // sees it. The sign goes on X by convention — any single axis rebuilds the
    // same matrix, and keeping it on one axis preserves the sign of the PRODUCT,
    // which is what buildMeshShape() reads to flip its triangle winding.
    if (glm::determinant(glm::mat3(m)) < 0.0f)
    {
        out.scale.x = -out.scale.x;
        axis[0]     = -axis[0];
    }

    out.rotation = glm::normalize(glm::quat_cast(glm::mat3(axis[0], axis[1], axis[2])));
    return out;
}

// The pose an entity actually stands at, parents included.
WorldPose worldPoseOf(HorizonWorld& world, Entity entity)
{
    return decomposeWorld(HE::worldMatrixOf(world, entity));
}

// The other direction, and the half that is easy to forget: what comes back out
// of Jolt is a WORLD pose, and a local one is where TransformComponent keeps it.
// A child written back raw walks away from its parent by the parent's offset on
// every single step, visibly, until it leaves the level.
//
// NOT COVERED, deliberately: the rotation half is exact only while the parent
// chain is free of NON-UNIFORM scale. Non-uniform scale shears a child, and a
// sheared basis has no rotation to extract — what comes out below is the closest
// rotation to it, so a physics-driven child of a squashed parent renders at a
// slightly different angle than its collider. Making that exact means storing a
// matrix per transform, which is a change to the transform format rather than to
// physics.
void writeBackWorldPose(HorizonWorld& world, Entity entity, TransformComponent& transform,
                        const glm::vec3& worldPos, const glm::quat& worldRot)
{
    // Position through the shared helper rather than a local copy of "invert the
    // parent": a second copy of that maths is exactly the drift
    // TransformHierarchy exists to prevent. It early-outs for a top-level entity,
    // which is the ordinary case — HorizonWorld::createEntity parents everything
    // to the world root, and the root carries no transform of its own.
    transform.position = HE::localPositionForWorld(world, entity, worldPos);

    glm::quat       localRot = worldRot;
    entt::registry& reg      = world.registry();
    const auto*     h        = reg.try_get<HierarchyComponent>(entity);
    const Entity    parent   = h ? h->parent : entt::null;
    if (parent != entt::null && parent != world.rootEntity() && reg.valid(parent))
        localRot = glm::normalize(glm::inverse(worldPoseOf(world, parent).rotation) * worldRot);

    transform.rotation = glm::degrees(glm::eulerAngles(localRot));
    transform.dirty    = true;
}

} // namespace

// ─── Collision shapes ─────────────────────────────────────────────────────────
namespace {

// A built collider plus the one thing the caller cannot see from the shape: two
// of the six shapes are surfaces, not solids, and Jolt cannot simulate them as
// anything but static (MeshShape::MustBeStatic and HeightFieldShape::
// MustBeStatic both return true). Jolt does not ENFORCE that — it simply
// integrates a body with meaningless inertia — so the engine has to.
struct ColliderBuild
{
    JPH::ShapeSettings::ShapeResult result;
    bool                            mustBeStatic = false;
};

// The mesh a collider is built from: LOD0 when the entity has LODs, otherwise
// MeshComponent's own id.
//
// NOT MeshComponent::meshAssetId on a LOD entity: LODSystem overwrites that
// every frame with whatever level suits the camera distance, so a collider built
// from it would change shape as the player walks towards it. NavigationSystem
// takes LOD0 for the same reason, and physics and navigation disagreeing about
// the shape of the world is precisely the defect this is fixing.
const StaticMeshAsset* colliderSourceMesh(const entt::registry& reg, entt::entity entity,
                                          ContentManager* content)
{
    if (!content)
        return nullptr;

    HE::UUID meshId{};
    if (const auto* mc = reg.try_get<MeshComponent>(entity))
        meshId = mc->meshAssetId;
    if (const auto* lod = reg.try_get<LODComponent>(entity); lod && !lod->levels.empty())
        meshId = lod->levels.front().meshId;
    if (meshId == HE::UUID{})
        return nullptr;

    return content->getStaticMesh(meshId);
}

// Positions of a mesh asset, in whichever of the two layouts it carries: cooked
// (packaged) meshes are interleaved pos3+norm3+uv2, loose (editor) ones keep
// tightly packed positions. Same two cases NavigationSystem walks.
struct MeshPositions
{
    const float* data   = nullptr;
    std::size_t  count  = 0;
    std::size_t  stride = 0;
};

MeshPositions meshPositions(const StaticMeshAsset& mesh)
{
    MeshPositions p;
    if (mesh.cooked && !mesh.interleaved.empty())
    {
        p.count  = mesh.vertexCount;
        p.stride = 8;
        if (mesh.interleaved.size() < p.count * p.stride)
            return {};   // truncated cooked blob — better no shape than garbage
        p.data = mesh.interleaved.data();
    }
    else if (!mesh.vertices.empty())
    {
        p.data   = mesh.vertices.data();
        p.count  = mesh.vertices.size() / 3;
        p.stride = 3;
    }
    return p;
}

glm::vec3 scaledVertex(const MeshPositions& p, std::size_t i, const glm::vec3& scale)
{
    return { p.data[i * p.stride + 0] * scale.x,
             p.data[i * p.stride + 1] * scale.y,
             p.data[i * p.stride + 2] * scale.z };
}

// Box that encloses the scaled mesh. The fallback for every mesh-shaped collider
// that cannot be built — an approximate body still stops the player, an absent
// one drops them through the floor with nothing on screen to explain it.
JPH::ShapeSettings::ShapeResult boxFromMeshBounds(const MeshPositions& p, const glm::vec3& scale)
{
    // A Jolt box is centred on the body's own origin, and a mesh's bounds need
    // not be — so the extent is taken from the furthest vertex in each direction
    // rather than from the box's width. That over-covers an off-centre mesh,
    // which is the right way round for a fallback: too much collision is
    // noticeable and fixable, too little is a player falling through the world.
    glm::vec3 half(0.01f);
    for (std::size_t i = 0; i < p.count; ++i)
        half = glm::max(half, glm::abs(scaledVertex(p, i, scale)));
    return JPH::BoxShapeSettings(JPH::Vec3(half.x, half.y, half.z)).Create();
}

// Triangle mesh collider. Exact, static-only, and the right answer for level
// geometry: a house imported from glTF is a house, not the box around it.
JPH::ShapeSettings::ShapeResult buildMeshShape(const StaticMeshAsset& mesh, const glm::vec3& scale)
{
    JPH::ShapeSettings::ShapeResult failed;

    const MeshPositions p = meshPositions(mesh);
    if (!p.data || p.count == 0 || mesh.indices.size() < 3)
    {
        failed.SetError("mesh asset carries no triangles on the CPU");
        return failed;
    }

    JPH::VertexList verts;
    verts.reserve(p.count);
    for (std::size_t i = 0; i < p.count; ++i)
    {
        const glm::vec3 v = scaledVertex(p, i, scale);
        verts.push_back(JPH::Float3(v.x, v.y, v.z));
    }

    // Jolt treats mesh triangles as SINGLE SIDED and expects counter-clockwise
    // winding. A mirrored scale (odd number of negative axes) reverses it, and
    // the result would be a house you can walk into but not out of.
    const bool flipped = (scale.x * scale.y * scale.z) < 0.0f;

    JPH::IndexedTriangleList tris;
    tris.reserve(mesh.indices.size() / 3);
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
    {
        const uint32_t a = mesh.indices[i + 0];
        const uint32_t b = mesh.indices[i + 1];
        const uint32_t c = mesh.indices[i + 2];
        if (a >= p.count || b >= p.count || c >= p.count)
            continue;   // malformed asset — drop the triangle, keep the mesh
        tris.push_back(JPH::IndexedTriangle(a, flipped ? c : b, flipped ? b : c));
    }
    if (tris.empty())
    {
        failed.SetError("every triangle of the mesh asset was degenerate or out of range");
        return failed;
    }

    return JPH::MeshShapeSettings(std::move(verts), std::move(tris)).Create();
}

// Convex hull of the same triangles. What Mesh cannot be: dynamic. A crate, a
// rock, a piece of debris — anything that has to fall and tumble but is not
// honestly a box.
JPH::ShapeSettings::ShapeResult buildConvexHullShape(const StaticMeshAsset& mesh,
                                                     const glm::vec3&       scale,
                                                     uint32_t               entityId)
{
    JPH::ShapeSettings::ShapeResult failed;

    const MeshPositions p = meshPositions(mesh);
    if (!p.data || p.count == 0)
    {
        failed.SetError("mesh asset carries no vertices on the CPU");
        return failed;
    }

    JPH::Array<JPH::Vec3> points;
    points.reserve(p.count);
    for (std::size_t i = 0; i < p.count; ++i)
    {
        const glm::vec3 v = scaledVertex(p, i, scale);
        points.push_back(JPH::Vec3(v.x, v.y, v.z));
    }

    // A hull may hold at most ConvexHullShape::cMaxPointsInHull (256) points, and
    // Jolt reports that as an ERROR rather than simplifying — a detailed asset
    // would otherwise end up with no body at all. mHullTolerance is how far a
    // point may sit outside the hull, so raising it is exactly "give me a coarser
    // hull"; the ladder below asks for the tightest one that fits.
    for (const float tolerance : { 1.0e-3f, 0.05f, 0.25f })
    {
        JPH::ConvexHullShapeSettings settings(points);
        settings.mHullTolerance = tolerance;
        JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (!result.HasError())
        {
            if (tolerance > 1.0e-3f)
                HE_LOG_WARN(Physics, "Entity %u: convex hull needed a %.2f m tolerance to fit "
                                     "Jolt's %d-point limit — the collider is coarser than the mesh",
                            entityId, static_cast<double>(tolerance),
                            JPH::ConvexHullShape::cMaxPointsInHull);
            return result;
        }
        failed = std::move(result);
    }
    return failed;
}

// Static collider for a landscape, straight from the height field the chunk
// meshes are generated from.
//
// ONE field for the whole terrain, not one per chunk. Three reasons, all of them
// in the surrounding code: the chunk meshes carry downward SKIRTS to hide LOD
// cracks (as colliders those are invisible walls at every chunk seam), the chunk
// entities are destroyed and rebuilt whenever the grid changes while a terrain
// body outlives that, and the body budget is 1024 for the entire world.
//
// `worldScale` is the landscape entity's scale from its world matrix, and it has
// to be applied HERE rather than left to the body: the chunk meshes are child
// entities, so they inherit that scale through the hierarchy and are drawn at it.
// A field built from tc.sizeX/sizeZ alone gave a scaled landscape visible ground
// in one place and collision in another.
JPH::ShapeSettings::ShapeResult buildHeightFieldShape(const TerrainComponent& tc,
                                                      const glm::vec3&        worldScale,
                                                      uint32_t                entityId)
{
    // TerrainSystem snaps the authored resolution to 2ⁿ+1 so chunk LOD0 vertices
    // land exactly on field samples. Reproduce that here on a LOCAL COPY: the
    // physics build can run before the terrain has ever ticked (initialize() at
    // scene start), and a component this class mutated behind TerrainSystem's
    // back would be a second author of the same data.
    uint32_t res   = std::clamp(tc.resolution, 2u, 1024u);
    uint32_t cells = res - 1, p = 1;
    while (p < cells) p <<= 1;
    const uint32_t snapped = p + 1;

    std::vector<float> field = computeTerrainHeightField(tc);
    if (snapped != res && field.size() == static_cast<std::size_t>(res) * res)
    {
        field = resampleHeightField(field, res, snapped);
        res   = snapped;
    }

    // Jolt needs sampleCount / blockSize >= 2 and the smallest block size is 2,
    // so a 2×2 field (a single quad) is below its floor. Resample rather than
    // refuse: the terrain is a legal one, it is just tiny.
    if (res < 3 && field.size() == static_cast<std::size_t>(res) * res)
    {
        field = resampleHeightField(field, res, 3);
        res   = 3;
    }

    JPH::ShapeSettings::ShapeResult failed;
    if (field.size() != static_cast<std::size_t>(res) * res)
    {
        failed.SetError("terrain height field size does not match its resolution");
        return failed;
    }

    // A MIRRORED landscape is not representable here: the field is a grid of
    // heights, so a negative X or Z scale would flip the surface's normals and
    // drop the player through a floor that is visibly there. Magnitudes, and say
    // so — a silent mirror is the failure this whole build path was audited for.
    if (worldScale.x < 0.0f || worldScale.y < 0.0f || worldScale.z < 0.0f)
        HE_LOG_WARN(Physics, "Entity %u: a landscape with a negative scale cannot be mirrored "
                             "into a height field — the collider uses the magnitudes "
                             "(%.3f, %.3f, %.3f) and will not match a mirrored terrain",
                    entityId, static_cast<double>(worldScale.x),
                    static_cast<double>(worldScale.y), static_cast<double>(worldScale.z));
    // Clamped rather than trusted: a zero-scaled axis collapses the field into a
    // line, and Jolt builds that without complaint.
    const glm::vec3 s = glm::max(glm::abs(worldScale), glm::vec3(1.0e-4f));

    // Jolt's surface is offset + scale * (x, samples[y * n + x], y), and
    // computeTerrainHeightField is row-major z*res+x in world-Y metres — so x maps
    // to the field's x, y to its z, and the Y scale is the entity's alone.
    const float stepX = tc.sizeX * s.x / static_cast<float>(res - 1);
    const float stepZ = tc.sizeZ * s.z / static_cast<float>(res - 1);
    JPH::HeightFieldShapeSettings settings(
        field.data(),
        JPH::Vec3(-tc.sizeX * 0.5f * s.x, 0.0f, -tc.sizeZ * 0.5f * s.z),
        JPH::Vec3(stepX, s.y, stepZ),
        res);
    settings.mBlockSize = 2;   // finest culling granularity Jolt allows

    return settings.Create();
}

// The one place that turns a ColliderComponent into a Jolt shape. initialize()
// and addEntity() both come through here, because two copies of this switch
// would be two engines' worth of collision behaviour within a release of each
// other.
//
// `worldScale` is the entity's scale AFTER its parents, not TransformComponent::
// scale — a prop inside a scaled zone is drawn at the composed scale, so that is
// the size its collider has to be.
//
// KNOWN LIMITATION, and it predates this: only the three shapes below that are
// built from geometry (Mesh, Convex Hull, Height Field) and the no-collider
// fallback take scale into account at all. Box, Sphere and Capsule use their
// AUTHORED dimensions and ignore scale entirely. Making those scale means
// wrapping every shape in a JPH::ScaledShape — one decision for all six, plus a
// migration for every scene whose colliders were authored around the current
// behaviour — so it is a deliberate hold, not an oversight.
ColliderBuild buildColliderShape(const entt::registry& reg,
                                 entt::entity          entity,
                                 const glm::vec3&      worldScale,
                                 ContentManager*       content)
{
    const uint32_t entityId = static_cast<uint32_t>(entity);
    const auto*    col      = reg.try_get<ColliderComponent>(entity);
    ColliderBuild  out;

    if (!col)
    {
        // abs(): decomposeWorld puts a mirror's sign on X, and a box has no
        // mirrored form — the signed value would clamp to a 0.01 m slab.
        const glm::vec3 halfEx = glm::max(glm::abs(worldScale) * 0.5f, glm::vec3(0.01f));
        out.result = JPH::BoxShapeSettings(JPH::Vec3(halfEx.x, halfEx.y, halfEx.z)).Create();
        return out;
    }

    // The box every failing branch falls back to. Authored half extents, so it
    // is at least the size the user drew in the inspector.
    const auto authoredBox = [col]() {
        return JPH::BoxShapeSettings(
            JPH::Vec3(std::max(0.01f, col->halfExtents.x),
                      std::max(0.01f, col->halfExtents.y),
                      std::max(0.01f, col->halfExtents.z))).Create();
    };

    switch (col->shape)
    {
    case ColliderShape::Sphere:
        out.result = JPH::SphereShapeSettings(std::max(0.01f, col->radius)).Create();
        break;

    case ColliderShape::Capsule: {
        const float halfCyl = std::max(0.0f, col->height * 0.5f - col->radius);
        out.result = JPH::CapsuleShapeSettings(halfCyl, std::max(0.01f, col->radius)).Create();
        break;
    }

    case ColliderShape::Mesh:
    case ColliderShape::ConvexHull: {
        const bool wantsMesh = col->shape == ColliderShape::Mesh;
        out.mustBeStatic     = wantsMesh;

        const StaticMeshAsset* mesh = colliderSourceMesh(reg, entity, content);
        if (!mesh)
        {
            HE_LOG_ERROR(Physics, "Entity %u: %s collider has no mesh to build from (%s) "
                                  "— falling back to a box collider",
                         entityId, wantsMesh ? "Mesh" : "Convex Hull",
                         content ? "the entity has no mesh asset, or it is not loaded"
                                 : "the physics world has no ContentManager");
            out.result       = authoredBox();
            out.mustBeStatic = false;
            break;
        }

        // The SIGNED world scale on purpose: buildMeshShape reads the sign of
        // its product to flip the triangle winding of a mirrored entity.
        out.result = wantsMesh ? buildMeshShape(*mesh, worldScale)
                               : buildConvexHullShape(*mesh, worldScale, entityId);
        if (out.result.HasError())
        {
            HE_LOG_ERROR(Physics, "Entity %u: %s collider could not be built (%s) "
                                  "— falling back to the mesh's bounding box",
                         entityId, wantsMesh ? "Mesh" : "Convex Hull",
                         out.result.GetError().c_str());
            const MeshPositions p = meshPositions(*mesh);
            out.result = (p.data && p.count > 0) ? boxFromMeshBounds(p, worldScale)
                                                 : authoredBox();
            out.mustBeStatic = false;
        }
        break;
    }

    case ColliderShape::HeightField: {
        const auto* tc = reg.try_get<TerrainComponent>(entity);
        if (!tc)
        {
            HE_LOG_ERROR(Physics, "Entity %u: Height Field collider on an entity with no "
                                  "TerrainComponent — falling back to a box collider",
                         entityId);
            out.result = authoredBox();
            break;
        }
        out.mustBeStatic = true;
        out.result       = buildHeightFieldShape(*tc, worldScale, entityId);
        if (out.result.HasError())
        {
            HE_LOG_ERROR(Physics, "Entity %u: terrain height field could not be built (%s) "
                                  "— falling back to a box collider",
                         entityId, out.result.GetError().c_str());
            out.result       = authoredBox();
            out.mustBeStatic = false;
        }
        break;
    }

    case ColliderShape::Box:
        out.result = authoredBox();
        break;

    default:
        // Was `default: // Box` for all three of the original shapes, which meant
        // every value added to the enum afterwards became a silent cube. That
        // reads as "physics ignores my setting" and has no log line to find.
        HE_LOG_ERROR(Physics, "Entity %u: unknown collider shape %u — falling back to a box "
                              "collider; PhysicsWorld needs a case for it",
                     entityId, static_cast<unsigned>(col->shape));
        out.result = authoredBox();
        break;
    }

    return out;
}

// Why a teleport (or a push) found nothing to act on. Spelled once, because it
// is read by whoever wrote the game script, not by whoever wrote the engine.
constexpr const char* kNoPhysicsReason =
    "the entity has neither a rigid body nor a character controller — add a "
    "RigidBodyComponent (or a CharacterControllerComponent) to give it one, or "
    "use transform.setPosition to move something that has no physics at all";

// Why a jump found nothing to jump with. Same audience as the line above.
constexpr const char* kNoCharacterReason =
    "it has no character controller — a jump moves a CharacterControllerComponent, "
    "so add one to the entity, or launch a rigid body with physics.addImpulse instead";

// The authoring component behind a character, or null. In one place because the
// jump reads it (jumpSpeed), gates on it (isGrounded, airTime) and mirrors back
// into it — three lookups that must not be able to disagree about validity.
CharacterControllerComponent* characterComponentOf(HorizonWorld* world, uint32_t entityId)
{
    if (!world) return nullptr;
    auto&        reg    = world->registry();
    const Entity entity = static_cast<Entity>(entityId);
    return reg.valid(entity) ? reg.try_get<CharacterControllerComponent>(entity) : nullptr;
}

// Coyote time: how long after walking off a ledge a jump is still granted.
// 0.12 s is about seven fixed steps — long enough to cover a player who pressed
// the button one frame late, short enough that nobody can read it as a second
// jump. See PhysicsWorld::jumpCharacter for why this is a constant and not an
// authored field.
constexpr float kCoyoteWindow = 0.12f;

// The four filters every CharacterVirtual query takes. Bundled because
// ExtendedUpdate in step() and RefreshContacts after a teleport must use the
// SAME ones — a character refreshed against a different filter set would resolve
// its ground against a different world than it walks in.
struct CharacterFilters
{
    JPH::DefaultBroadPhaseLayerFilter bp;
    JPH::DefaultObjectLayerFilter     ol;
    JPH::BodyFilter                   body;
    JPH::ShapeFilter                  shape;

    CharacterFilters(const ObjectVsBPLayerFilterImpl& ovbp, const ObjectLayerPairFilterImpl& oo)
        : bp(ovbp, HELayers::MOVING), ol(oo, HELayers::MOVING) {}
};

} // namespace

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

void PhysicsWorld::setContentManager(ContentManager* content)
{
    if (m_impl)
        m_impl->content = content;
}

bool PhysicsWorld::buildBodyFor(HorizonWorld& world, uint32_t entityId)
{
    auto&        reg    = world.registry();
    const Entity entity = static_cast<Entity>(entityId);
    if (!reg.valid(entity))
        return false;

    const auto* transform = reg.try_get<TransformComponent>(entity);
    const auto* rb        = reg.try_get<RigidBodyComponent>(entity);
    if (!transform || !rb)
        return false;

    // The WORLD pose, composed from the parent chain here and now — not
    // TransformComponent's own fields, which are LOCAL. Treating a local pose as
    // a world one is how an additively streamed zone ended up with its meshes at
    // the zone position and its colliders at the authored origin, and how a
    // prefab spawned at (100, 0, 100) gave its child a collider at (2, 0, 0)
    // instead of (102, 0, 100). It is built ONCE per body, here and in
    // addEntity() — step() never rebuilds — so the upward walk is paid per spawn,
    // not per frame.
    const WorldPose pose = worldPoseOf(world, entity);

    const auto* col   = reg.try_get<ColliderComponent>(entity);
    ColliderBuild build = buildColliderShape(reg, entity, pose.scale, m_impl->content);
    if (build.result.HasError())
    {
        HE_LOG_ERROR(Physics, "Entity %u: rigid-body collider shape could not be built (%s) "
                              "— entity has no physics body",
                     entityId, build.result.GetError().c_str());
        return false;
    }

    const JPH::Quat  jq { pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w };
    const JPH::RVec3 pos(pose.position.x, pose.position.y, pose.position.z);

    JPH::EMotionType motionType;
    JPH::ObjectLayer layer;
    switch (rb->type)
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

    // A triangle mesh and a height field are surfaces with no inside, so Jolt
    // cannot give them mass or inertia — but it does not refuse the body either,
    // it simulates it with nonsense. Downgrade loudly: an object that stopped
    // moving is a bug the author can see and fix, a body integrating garbage is
    // one they cannot.
    if (build.mustBeStatic && motionType != JPH::EMotionType::Static)
    {
        HE_LOG_WARN(Physics, "Entity %u: a %s rigid body cannot use this collider shape "
                             "(Jolt has no solver state for a surface) — forced to Static. "
                             "Use Convex Hull for a body that has to move.",
                    entityId, rb->type == RigidBodyType::Dynamic ? "Dynamic" : "Kinematic");
        motionType = JPH::EMotionType::Static;
        layer      = HELayers::NON_MOVING;
    }

    JPH::BodyCreationSettings bcs(build.result.Get(), pos, jq, motionType, layer);
    // Gated on the FINAL motion type, not on rb->type: after a downgrade, asking
    // Jolt to calculate inertia for a static mesh body is a request it cannot
    // satisfy.
    if (motionType == JPH::EMotionType::Dynamic)
    {
        bcs.mMassPropertiesOverride.mMass = rb->mass;
        bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    }
    bcs.mFriction    = rb->friction;
    bcs.mRestitution = rb->restitution;
    // ColliderComponent::isTrigger was authored, serialised and drawn in its
    // own debug colour, but never reached Jolt — so a trigger volume blocked
    // like a wall and produced no overlap events at all. A sensor passes
    // bodies through and still reports contacts, which is what the
    // OnBeginOverlap / OnEndOverlap pair is built from.
    bcs.mIsSensor = col && col->isTrigger;

    const JPH::EActivation activation = (motionType == JPH::EMotionType::Static)
        ? JPH::EActivation::DontActivate
        : JPH::EActivation::Activate;

    // Store entity ID in body user data for reverse lookup during raycasts.
    // This is the ONLY body → entity mapping: raycast, sphereCast, overlapSphere
    // and every collision event read it and nothing else. A body created without
    // it is invisible to all of them, silently.
    bcs.mUserData = static_cast<uint64_t>(entityId);

    const JPH::BodyID bodyId =
        m_impl->physicsSystem.GetBodyInterface().CreateAndAddBody(bcs, activation);
    if (bodyId.IsInvalid())
    {
        HE_LOG_ERROR(Physics, "Entity %u: body creation failed — the %u-body limit is "
                              "most likely exhausted (%zu created so far)",
                     entityId, Impl::kMaxBodies, m_impl->entityToBody.size());
        return false;
    }

    m_impl->entityToBody[entityId] = bodyId;
    return true;
}

bool PhysicsWorld::buildCharacterFor(HorizonWorld& world, uint32_t entityId)
{
    auto&        reg    = world.registry();
    const Entity entity = static_cast<Entity>(entityId);
    if (!reg.valid(entity))
        return false;

    const auto* transform = reg.try_get<TransformComponent>(entity);
    const auto* cc        = reg.try_get<CharacterControllerComponent>(entity);
    if (!transform || !cc)
        return false;

    // An entity carrying BOTH a CharacterController and a RigidBody is the
    // NORMAL case, not an exotic one: EntityHost::defaultComponents gives it to
    // every PlayerCharacter, because the kinematic body is the collision proxy
    // other bodies see and the character is what moves. Both are built.
    //
    // The two are kept from fighting elsewhere: the body write-back skips
    // character entities (the character owns the transform), the character
    // ignores its own body while stepping, and the body is dragged along after
    // (see step()).
    //
    // Only a capsule or a sphere collider is honoured here — a character IS a
    // capsule as far as Jolt's walk/step/slide solver is concerned, so a box or
    // a mesh collider on a character entity still yields the default capsule.
    //
    // The dimensions below are the AUTHORED ones; world scale is not applied,
    // exactly as for Box/Sphere/Capsule in buildColliderShape (see the limitation
    // noted there). A character scaled by its transform keeps a full-size capsule.
    JPH::ShapeSettings::ShapeResult shapeResult;
    const auto* col = reg.try_get<ColliderComponent>(entity);
    if (col && col->shape == ColliderShape::Capsule)
    {
        const float halfCyl = std::max(0.0f, col->height * 0.5f - col->radius);
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
                     entityId, shapeResult.GetError().c_str());
        return false;
    }

    JPH::CharacterVirtualSettings cvs;
    cvs.mMass             = cc->mass;
    cvs.mCharacterPadding = cc->skinWidth;
    cvs.mShape            = shapeResult.Get();
    cvs.mUp               = JPH::Vec3::sAxisY();
    cvs.mMaxSlopeAngle    = JPH::DegreesToRadians(cc->slopeLimit);

    // World pose, for the same reason buildBodyFor uses one: a character spawned
    // as part of a prefab or inside an additively loaded zone stands where the
    // hierarchy puts it, not where its own local transform says.
    const WorldPose  pose = worldPoseOf(world, entity);
    const JPH::RVec3 pos(pose.position.x, pose.position.y, pose.position.z);
    const JPH::Quat  jq { pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w };

    m_impl->entityToCharacter[entityId] = std::make_unique<JPH::CharacterVirtual>(
        &cvs, pos, jq, static_cast<uint64_t>(entityId), &m_impl->physicsSystem);
    return true;
}

bool PhysicsWorld::buildTerrainBodyFor(HorizonWorld& world, uint32_t entityId)
{
    auto&        reg    = world.registry();
    const Entity entity = static_cast<Entity>(entityId);
    if (!reg.valid(entity))
        return false;

    const auto* tc        = reg.try_get<TerrainComponent>(entity);
    const auto* transform = reg.try_get<TransformComponent>(entity);
    if (!tc || !transform)
        return false;
    // An authored rigid body wins: this is the fallback for the landscape
    // NOTHING authors physics for, and buildBodyFor already covers the other
    // case (including an explicit Height Field collider).
    if (reg.all_of<RigidBodyComponent>(entity))
        return false;

    // World pose again: the chunk meshes are CHILDREN of this entity, so they are
    // drawn through its world matrix. A field placed from the local transform
    // would sit under a landscape that is anywhere but the top level.
    const WorldPose pose = worldPoseOf(world, entity);

    JPH::ShapeSettings::ShapeResult shapeResult =
        buildHeightFieldShape(*tc, pose.scale, entityId);
    if (shapeResult.HasError())
    {
        HE_LOG_ERROR(Physics, "Entity %u: landscape collider could not be built (%s) "
                              "— things will fall through this terrain",
                     entityId, shapeResult.GetError().c_str());
        return false;
    }

    JPH::BodyCreationSettings bcs(
        shapeResult.Get(),
        JPH::RVec3(pose.position.x, pose.position.y, pose.position.z),
        JPH::Quat(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w),
        JPH::EMotionType::Static, HELayers::NON_MOVING);
    bcs.mFriction = 0.5f;   // RigidBodyComponent's default, so an authored body matches
    bcs.mUserData = static_cast<uint64_t>(entityId);

    const JPH::BodyID bodyId = m_impl->physicsSystem.GetBodyInterface().CreateAndAddBody(
        bcs, JPH::EActivation::DontActivate);
    if (bodyId.IsInvalid())
    {
        HE_LOG_ERROR(Physics, "Entity %u: landscape body creation failed — the %u-body limit "
                              "is most likely exhausted (%zu created so far)",
                     entityId, Impl::kMaxBodies, m_impl->entityToBody.size());
        return false;
    }

    m_impl->entityToBody[entityId] = bodyId;
    return true;
}

void PhysicsWorld::initialize(HorizonWorld& world)
{
    clear();

    m_impl->world = &world;
    auto& reg     = world.registry();

    // Every view is collected BEFORE anything is built. The builders reach for
    // components a scene may not have any storage for yet, and creating a pool
    // is exactly what invalidates a view that is being iterated.
    const auto collect = [](auto view) {
        std::vector<uint32_t> ids;
        for (auto entity : view)
            ids.push_back(static_cast<uint32_t>(entity));
        return ids;
    };

    const std::vector<uint32_t> bodies =
        collect(reg.view<TransformComponent, RigidBodyComponent>());
    const std::vector<uint32_t> characters =
        collect(reg.view<TransformComponent, CharacterControllerComponent>());
    // ── Landscapes ────────────────────────────────────────────────────────────
    // Nothing gives a terrain entity a RigidBodyComponent — not the Create
    // Landscape tool, not the scene loader, not the chunk generator. So every
    // outdoor level had a floor the AI could walk on (NavigationSystem bakes the
    // chunks into the navmesh) and the player fell straight through. The
    // landscape gets its collider here instead of waiting for someone to author
    // one, because a landscape you fall through is never what was meant.
    const std::vector<uint32_t> landscapes =
        collect(reg.view<TerrainComponent, TransformComponent>());

    for (uint32_t entityId : bodies)
        buildBodyFor(world, entityId);
    for (uint32_t entityId : characters)
        buildCharacterFor(world, entityId);
    std::size_t terrains = 0;
    for (uint32_t entityId : landscapes)
        if (buildTerrainBodyFor(world, entityId))
            ++terrains;

    m_impl->physicsSystem.OptimizeBroadPhase();
    m_impl->initialized = true;

    HE_LOG_INFO(Physics, "Physics world initialised: %zu rigid body/-ies (%zu landscape), "
                         "%zu character controller(s)",
                m_impl->entityToBody.size(), terrains, m_impl->entityToCharacter.size());
    if (m_impl->entityToBody.size() > Impl::kMaxBodies * 9 / 10)
        HE_LOG_WARN(Physics, "Body count %zu is close to the hard limit of %u",
                    m_impl->entityToBody.size(), Impl::kMaxBodies);
}

// ─── Runtime composition ──────────────────────────────────────────────────────

bool PhysicsWorld::addEntity(HorizonWorld& world, uint32_t entityId)
{
    if (!m_impl)
        return false;

    m_impl->world       = &world;
    auto&        reg    = world.registry();
    const Entity entity = static_cast<Entity>(entityId);
    if (!reg.valid(entity))
        return false;

    // Idempotent means REPLACE, and replace means tear the old one down first.
    // Overwriting the map entry alone would leave the previous Jolt body in the
    // world forever — invisible, blocking, and answering every raycast with this
    // same entity id.
    removeEntity(entityId);

    bool built = buildBodyFor(world, entityId);
    built      = buildCharacterFor(world, entityId) || built;
    if (!built)
        built = buildTerrainBodyFor(world, entityId);

    // Deliberately no OptimizeBroadPhase() here: it rebuilds the entire broad
    // phase tree, and Jolt inserts single bodies incrementally on purpose. Doing
    // it per spawn would make firing a weapon cost a full scene rebuild.

    if (built && !m_impl->initialized)
        HE_LOG_THROTTLE(Physics, Warning, 5.0,
                        "Entity %u was given a physics body before the world was initialised — "
                        "it will not simulate, and initialize() discards it", entityId);
    return built;
}

int PhysicsWorld::addEntityTree(HorizonWorld& world, uint32_t rootEntityId)
{
    if (!m_impl)
        return 0;

    auto&        reg  = world.registry();
    const Entity root = static_cast<Entity>(rootEntityId);
    if (!reg.valid(root))
        return 0;

    // A spawn is a subtree, not an entity: a PlayerCharacter prefab brings child
    // entities, and their colliders are as much part of "the thing that was
    // spawned" as the root's.
    int built = 0;
    std::vector<Entity> pending{ root };
    while (!pending.empty())
    {
        const Entity current = pending.back();
        pending.pop_back();
        if (!reg.valid(current))
            continue;
        if (addEntity(world, static_cast<uint32_t>(current)))
            ++built;
        if (const auto* hierarchy = reg.try_get<HierarchyComponent>(current))
            for (Entity child : hierarchy->children)
                pending.push_back(child);
    }
    return built;
}

void PhysicsWorld::removeEntity(uint32_t entityId)
{
    if (!m_impl)
        return;
    destroyBodyFor(entityId);
    // A CharacterVirtual has no Jolt body of its own (mInnerBodyShape is never
    // set), so it appears in no raycast, overlap or contact — erasing the owning
    // pointer IS its complete removal.
    m_impl->entityToCharacter.erase(entityId);
}

int PhysicsWorld::removeEntityTree(HorizonWorld& world, uint32_t rootEntityId)
{
    if (!m_impl)
        return 0;

    auto&        reg  = world.registry();
    const Entity root = static_cast<Entity>(rootEntityId);
    if (!reg.valid(root))
    {
        // The handle is already gone, so the hierarchy cannot be walked. Remove
        // what we can name and let step()'s reap collect the rest.
        const bool had = hasPhysics(rootEntityId);
        removeEntity(rootEntityId);
        return had ? 1 : 0;
    }

    int removed = 0;
    std::vector<Entity> pending{ root };
    while (!pending.empty())
    {
        const Entity current = pending.back();
        pending.pop_back();
        if (!reg.valid(current))
            continue;
        const uint32_t id = static_cast<uint32_t>(current);
        if (hasPhysics(id))
        {
            removeEntity(id);
            ++removed;
        }
        if (const auto* hierarchy = reg.try_get<HierarchyComponent>(current))
            for (Entity child : hierarchy->children)
                pending.push_back(child);
    }
    return removed;
}

bool PhysicsWorld::hasPhysics(uint32_t entityId) const
{
    if (!m_impl)
        return false;
    return m_impl->entityToBody.count(entityId) != 0 ||
           m_impl->entityToCharacter.count(entityId) != 0;
}

bool PhysicsWorld::hasCharacter(uint32_t entityId) const
{
    if (!m_impl)
        return false;
    return m_impl->entityToCharacter.count(entityId) != 0;
}

void PhysicsWorld::destroyBodyFor(uint32_t entityId)
{
    const auto it = m_impl->entityToBody.find(entityId);
    if (it == m_impl->entityToBody.end())
        return;

    // Remove THEN destroy — Jolt's own order, and the one clear() has always used.
    auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
    bodyInterface.RemoveBody(it->second);
    bodyInterface.DestroyBody(it->second);
    m_impl->entityToBody.erase(it);

    // And forget its contacts. Jolt fires OnContactRemoved for a destroyed body's
    // cached contacts during the NEXT Update(); without this the listener would
    // resolve them from its cache and hand game code an exit event for an entity
    // that is already gone. clear() does the same thing for the whole world.
    m_impl->contactListener.purgeEntity(entityId);
}

bool PhysicsWorld::setPosition(uint32_t entityId, const glm::vec3& position, bool resetVelocity)
{
    if (!m_impl)
        return false;

    // Position ONLY, and written straight rather than routed through
    // setTransform with a rotation read back from Jolt. A CharacterVirtual's
    // rotation is never written by anything — step() writes its position back
    // and the proxy takes its orientation from the TransformComponent — so
    // GetRotation() still answers with the spawn pose for the rest of the
    // session. Feeding that back would spin the player round to face their
    // starting direction on every respawn.
    const JPH::RVec3 pos(position.x, position.y, position.z);

    const auto characterIt = m_impl->entityToCharacter.find(entityId);
    const auto bodyIt      = m_impl->entityToBody.find(entityId);
    if (characterIt == m_impl->entityToCharacter.end() && bodyIt == m_impl->entityToBody.end())
    {
        HE_LOG_THROTTLE(Physics, Warning, 5.0,
                        "physics.setPosition on entity %u did nothing: %s",
                        entityId, kNoPhysicsReason);
        return false;
    }

    if (characterIt != m_impl->entityToCharacter.end())
    {
        JPH::CharacterVirtual& character = *characterIt->second;
        character.SetPosition(pos);
        if (resetVelocity)
            character.SetLinearVelocity(JPH::Vec3::sZero());

        // See setTransform: without this the character keeps the ground contacts
        // of the place it left.
        CharacterFilters filters(m_impl->ovbpFilter, m_impl->ooFilter);
        character.RefreshContacts(filters.bp, filters.ol, filters.body, filters.shape,
                                  m_impl->tempAllocator);
    }

    // Both, when the entity has both: the character is what moves, the kinematic
    // body is what everything else collides with, and a proxy left behind is the
    // invisible copy of the player this class already had to fix once.
    if (bodyIt != m_impl->entityToBody.end())
    {
        auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
        bodyInterface.SetPosition(bodyIt->second, pos, JPH::EActivation::Activate);
        if (resetVelocity && bodyInterface.GetMotionType(bodyIt->second) != JPH::EMotionType::Static)
            bodyInterface.SetLinearAndAngularVelocity(bodyIt->second,
                                                      JPH::Vec3::sZero(), JPH::Vec3::sZero());
    }

    if (m_impl->world)
    {
        auto&        reg    = m_impl->world->registry();
        const Entity entity = static_cast<Entity>(entityId);
        if (reg.valid(entity))
        {
            if (auto* transform = reg.try_get<TransformComponent>(entity))
            {
                // `position` is a WORLD position — that is the only kind Jolt
                // took above — and TransformComponent stores a local one.
                // Assigning it raw would teleport a parented entity to the world
                // point PLUS its parent's offset, and every subsequent read of
                // its position would disagree with where physics put it.
                transform->position =
                    HE::localPositionForWorld(*m_impl->world, entity, position);
                transform->dirty = true;
            }
            // Spend the coyote credit: a teleport is not a step off a ledge, and
            // the ground the character was standing on is not under it any more.
            // Without this, a player respawned into mid-air could still jump off
            // nothing for the length of the window. The next step re-earns it if
            // they arrived on solid ground.
            if (auto* cc = reg.try_get<CharacterControllerComponent>(entity))
                cc->airTime = kCoyoteWindow;
        }
    }

    return true;
}

bool PhysicsWorld::setTransform(uint32_t entityId, const glm::vec3& position,
                                const glm::quat& rotation, bool resetVelocity)
{
    if (!m_impl)
        return false;

    const JPH::RVec3 pos(position.x, position.y, position.z);
    const JPH::Quat  rot(rotation.x, rotation.y, rotation.z, rotation.w);

    const auto characterIt = m_impl->entityToCharacter.find(entityId);
    const auto bodyIt      = m_impl->entityToBody.find(entityId);
    if (characterIt == m_impl->entityToCharacter.end() && bodyIt == m_impl->entityToBody.end())
    {
        HE_LOG_THROTTLE(Physics, Warning, 5.0,
                        "teleporting entity %u did nothing: %s",
                        entityId, kNoPhysicsReason);
        return false;
    }

    if (characterIt != m_impl->entityToCharacter.end())
    {
        JPH::CharacterVirtual& character = *characterIt->second;
        character.SetPosition(pos);
        character.SetRotation(rot);
        if (resetVelocity)
            character.SetLinearVelocity(JPH::Vec3::sZero());

        // Mandatory after moving a character: its contact list still describes
        // the floor it was standing on. Without this, IsSupported() and
        // GetGroundState() answer for the OLD place — a player teleported off a
        // ledge would keep reporting solid ground and never start falling.
        CharacterFilters filters(m_impl->ovbpFilter, m_impl->ooFilter);
        character.RefreshContacts(filters.bp, filters.ol, filters.body, filters.shape,
                                  m_impl->tempAllocator);
    }

    if (bodyIt != m_impl->entityToBody.end())
    {
        auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
        // SetPositionAndRotation, never MoveKinematic. MoveKinematic is what
        // step() uses to DRAG a character's collision proxy along, i.e. a
        // movement over the step that collides with everything on the way — over
        // a teleport's distance it would shove the whole level about. Activate is
        // not optional either: a sleeping dynamic body would otherwise hang
        // motionless at its new position until something touched it.
        bodyInterface.SetPositionAndRotation(bodyIt->second, pos, rot, JPH::EActivation::Activate);
        if (resetVelocity && bodyInterface.GetMotionType(bodyIt->second) != JPH::EMotionType::Static)
            bodyInterface.SetLinearAndAngularVelocity(bodyIt->second,
                                                      JPH::Vec3::sZero(), JPH::Vec3::sZero());
    }

    // And the ECS side, in the same call. Between a teleport and the next step
    // sit the camera update and render extraction in the game, and the whole
    // script phase in the editor — leaving the transform behind would draw the
    // respawned player at the place they just died for a frame, and hand any
    // script that reads its position the old one.
    //
    // Through writeBackWorldPose, because the pair above is a WORLD pose (Jolt
    // takes nothing else) and the transform holds a local one.
    if (m_impl->world)
    {
        auto&        reg    = m_impl->world->registry();
        const Entity entity = static_cast<Entity>(entityId);
        if (reg.valid(entity))
        {
            if (auto* transform = reg.try_get<TransformComponent>(entity))
                writeBackWorldPose(*m_impl->world, entity, *transform, position, rotation);
            // Spend the coyote credit — see setPosition for why a teleport ends
            // the grace period rather than carrying it to the new place.
            if (auto* cc = reg.try_get<CharacterControllerComponent>(entity))
                cc->airTime = kCoyoteWindow;
        }
    }

    return true;
}

void PhysicsWorld::step(HorizonWorld& world, float dt)
{
    if (!m_impl->initialized || dt <= 0.0f)
        return;

    // A physics step that overruns this badly stalls the whole frame; throttled so
    // a permanently overloaded scene reports once a second, not 60 times.
    HE_LOG_SLOW_SCOPE(Physics, 8.0, "PhysicsWorld::step");

    m_impl->world = &world;

    m_impl->physicsSystem.Update(dt, 1,
        &m_impl->tempAllocator, &m_impl->jobSystem);

    auto& reg           = world.registry();
    auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();

    // Snapshot the ids before touching anything. The write-back below runs game
    // code's contact handlers indirectly and may itself remove entries (the reap),
    // and either would rehash the map out from under a range-for.
    m_impl->stepBodies.clear();
    m_impl->stepBodies.reserve(m_impl->entityToBody.size());
    for (const auto& entry : m_impl->entityToBody)
        m_impl->stepBodies.push_back(entry.first);

    for (const uint32_t entityId : m_impl->stepBodies)
    {
        auto bodyIt = m_impl->entityToBody.find(entityId);
        if (bodyIt == m_impl->entityToBody.end())
            continue;   // removed since the snapshot was taken
        const JPH::BodyID bodyId = bodyIt->second;

        // The reap, and it has to come BEFORE the static-body skip: nothing
        // notifies this class when an entity is deleted, and a static body whose
        // entity is gone would otherwise never be looked at again — an invisible
        // wall for the rest of the session, answering raycasts with a dead id.
        Entity entity = static_cast<Entity>(entityId);
        if (!reg.valid(entity))
        {
            removeEntity(entityId);
            continue;
        }

        // Only write back non-static bodies
        if (bodyInterface.GetMotionType(bodyId) == JPH::EMotionType::Static)
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

        // GetPosition, not GetCenterOfMassPosition: a body is CREATED at the
        // entity's world position, so that is the value this has to read back. The
        // two agree for a box, a sphere and a capsule — every shape that existed
        // when this was written — but a convex hull's centre of mass sits
        // wherever the geometry puts it, and reading that would shift the mesh
        // by the offset on the very first step and every step after.
        JPH::RVec3 pos = bodyInterface.GetPosition(bodyId);
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

        // WORLD pose out of Jolt, LOCAL pose into the transform. Without the
        // conversion a parented body gains its parent's offset every step and
        // visibly drifts away from the thing it is attached to — which is why
        // this is the other half of building in world space, not a follow-up.
        //
        // NOT COVERED: stepBodies is an unordered snapshot, so a dynamic body
        // whose PARENT is also dynamic may be converted against the parent's
        // pose from the previous frame. The error is bounded by one frame of the
        // parent's movement and cannot accumulate — every frame re-derives the
        // local pose from Jolt's world pose, which was never wrong. Sorting the
        // snapshot by hierarchy depth would fix it and cost every scene a sort
        // per step for a case (rigid bodies parented to rigid bodies) the engine
        // does not otherwise support.
        writeBackWorldPose(world, entity, *transform,
                           glm::vec3(static_cast<float>(pos.GetX()),
                                     static_cast<float>(pos.GetY()),
                                     static_cast<float>(pos.GetZ())),
                           glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ()));
    }

    // ── Character controller update ────────────────────────────────────────────
    CharacterFilters filters(m_impl->ovbpFilter, m_impl->ooFilter);
    JPH::CharacterVirtual::ExtendedUpdateSettings euSettings;

    m_impl->stepCharacters.clear();
    m_impl->stepCharacters.reserve(m_impl->entityToCharacter.size());
    for (const auto& entry : m_impl->entityToCharacter)
        m_impl->stepCharacters.push_back(entry.first);

    for (const uint32_t entityId : m_impl->stepCharacters)
    {
        const auto characterIt = m_impl->entityToCharacter.find(entityId);
        if (characterIt == m_impl->entityToCharacter.end())
            continue;   // removed since the snapshot was taken
        JPH::CharacterVirtual* character = characterIt->second.get();

        Entity entity = static_cast<Entity>(entityId);
        if (!reg.valid(entity))
        {
            removeEntity(entityId);   // the reap again — see the body loop
            continue;
        }

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
            filters.bp, filters.ol, selfFilter, filters.shape,
            m_impl->tempAllocator);

        // Sync position back to transform. POSITION ONLY, and not through
        // writeBackWorldPose: a CharacterVirtual's rotation is never written by
        // anything (the transform owns the character's facing — see setPosition),
        // so converting and storing it would pin the character to its spawn
        // heading. The position still needs converting, because Jolt's is a world
        // position and the transform's is local.
        JPH::RVec3      pos = character->GetPosition();
        const glm::vec3 worldPos(static_cast<float>(pos.GetX()),
                                 static_cast<float>(pos.GetY()),
                                 static_cast<float>(pos.GetZ()));
        transform->position = HE::localPositionForWorld(world, entity, worldPos);
        transform->dirty    = true;

        // Sync velocity and ground state back to component
        JPH::Vec3 newVel = character->GetLinearVelocity();
        cc->velocity   = { newVel.GetX(), newVel.GetY(), newVel.GetZ() };
        cc->isGrounded = (character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround);

        // The coyote credit, in SIMULATED time: it accrues per fixed step, so it
        // measures the same grace whatever the frame rate. Landing restores it.
        cc->airTime = cc->isGrounded ? 0.0f : cc->airTime + dt;

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
        //
        // The proxy is an ordinary Jolt body, so it takes the WORLD pose: the
        // position the character just reported (already world) and the facing the
        // transform holds composed with its parents'. Handing it the local fields
        // would leave a parented character's collision proxy behind at the
        // parent's offset — the ghost this block exists to prevent, moved rather
        // than removed.
        if (bodyIt != m_impl->entityToBody.end())
        {
            const glm::quat q = worldPoseOf(world, entity).rotation;
            bodyInterface.MoveKinematic(bodyIt->second,
                JPH::RVec3(worldPos.x, worldPos.y, worldPos.z),
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

// Collects the ENTITIES an overlap test touched, rather than the hits.
//
// It has to be a custom collector: on the CollideShape path Jolt never fills
// CollideShapeResult::mBodyID2 (only the shape-cast path does), so the hit
// alone cannot say what was hit. The body is knowable exactly while it is being
// reported, and Jolt offers it as OnBody() right before the narrow-phase test —
// no body lock needed, unlike reading it back afterwards.
//
// Dedup lives here for the same reason it is needed at all: one body reports
// once per sub shape (compound and mesh shapes), and "the crate is in the
// blast" is true exactly once. Insertion order is the query's own.
class HEOverlapCollector final : public JPH::CollideShapeCollector
{
public:
    void OnBody(const JPH::Body& inBody) override
    {
        m_current = static_cast<uint32_t>(inBody.GetUserData());
    }

    void AddHit(const JPH::CollideShapeResult&) override
    {
        if (m_current == PhysicsWorld::kNoEntity)
            return;
        if (m_seen.insert(m_current).second)
            m_entities.push_back(m_current);
    }

    std::vector<uint32_t> take() { return std::move(m_entities); }

private:
    uint32_t                     m_current = PhysicsWorld::kNoEntity;
    std::unordered_set<uint32_t> m_seen;
    std::vector<uint32_t>        m_entities;
};

// The body a script's push should land on, plus why it cannot land when it
// does not. The verdict is separated from the LOGGING so every public call can
// name itself in its own throttled message — one shared log site would let
// whichever call fired first silence all the others for the whole cooldown.
struct BodyTarget
{
    JPH::BodyID id;
    bool        exists  = false;   // the entity has a body at all
    bool        dynamic = false;   // …and the solver will accept a force on it
    bool        movable = false;   // …or at least a velocity (kinematic counts)
};

BodyTarget bodyTarget(const std::unordered_map<uint32_t, JPH::BodyID>& bodies,
                      const JPH::BodyInterface&                        bodyInterface,
                      uint32_t                                         entityId)
{
    BodyTarget target;
    const auto it = bodies.find(entityId);
    if (it == bodies.end())
        return target;
    const JPH::EMotionType motion = bodyInterface.GetMotionType(it->second);
    target.id      = it->second;
    target.exists  = true;
    target.dynamic = motion == JPH::EMotionType::Dynamic;
    target.movable = motion != JPH::EMotionType::Static;
    return target;
}

// Both refusals, spelled once. They say what to DO about it, because whoever
// reads them wrote a game script, not the engine.
constexpr const char* kNoBodyReason =
    "the entity has no physics body — a body is built from a RigidBodyComponent, "
    "at scene start or when the entity is spawned, so an entity that has no such "
    "component has none";
constexpr const char* kNotDynamicReason =
    "its rigid body is not Dynamic — a Static or Kinematic body has no solver "
    "state to push";

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

std::vector<uint32_t> PhysicsWorld::overlapSphere(
    const glm::vec3& center,
    float            radius,
    uint32_t         ignoreEntityId) const
{
    std::vector<uint32_t> result;
    if (!m_impl || !m_impl->initialized || radius <= 0.0f)
        return result;

    JPH::Ref<JPH::Shape> sphere = new JPH::SphereShape(radius);
    const JPH::RVec3     at(center.x, center.y, center.z);

    JPH::CollideShapeSettings settings;
    HEOverlapCollector        collector;
    const HEQueryFilter       bodyFilter(ignoreEntityId, /*skipSensors=*/false);

    // Results are expressed relative to `at` rather than the world origin, the
    // same reason sphereCast passes its origin as the base offset: it is what
    // keeps the test itself precise far from the origin.
    m_impl->physicsSystem.GetNarrowPhaseQuery().CollideShape(
        sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(at),
        settings, at, collector, {}, {}, bodyFilter);

    return collector.take();
}

bool PhysicsWorld::addForce(uint32_t entityId, const glm::vec3& force)
{
    if (!m_impl) return false;

    auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
    const BodyTarget target = bodyTarget(m_impl->entityToBody, bodyInterface, entityId);
    if (!target.dynamic)
    {
        HE_LOG_THROTTLE(Physics, Warning, 5.0, "addForce on entity %u did nothing: %s",
                        entityId, target.exists ? kNotDynamicReason : kNoBodyReason);
        return false;
    }
    bodyInterface.AddForce(target.id, JPH::Vec3(force.x, force.y, force.z));
    return true;
}

bool PhysicsWorld::addImpulse(uint32_t entityId, const glm::vec3& impulse)
{
    if (!m_impl) return false;

    auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
    const BodyTarget target = bodyTarget(m_impl->entityToBody, bodyInterface, entityId);
    if (!target.dynamic)
    {
        HE_LOG_THROTTLE(Physics, Warning, 5.0, "addImpulse on entity %u did nothing: %s",
                        entityId, target.exists ? kNotDynamicReason : kNoBodyReason);
        return false;
    }
    bodyInterface.AddImpulse(target.id, JPH::Vec3(impulse.x, impulse.y, impulse.z));
    return true;
}

bool PhysicsWorld::addTorque(uint32_t entityId, const glm::vec3& torque)
{
    if (!m_impl) return false;

    auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
    const BodyTarget target = bodyTarget(m_impl->entityToBody, bodyInterface, entityId);
    if (!target.dynamic)
    {
        HE_LOG_THROTTLE(Physics, Warning, 5.0, "addTorque on entity %u did nothing: %s",
                        entityId, target.exists ? kNotDynamicReason : kNoBodyReason);
        return false;
    }
    bodyInterface.AddTorque(target.id, JPH::Vec3(torque.x, torque.y, torque.z));
    return true;
}

bool PhysicsWorld::setVelocity(uint32_t entityId, const glm::vec3& velocity)
{
    if (!m_impl) return false;

    // The character first — see the header for why the two representations
    // share one call and why this order is the one that changes nothing.
    const auto character = m_impl->entityToCharacter.find(entityId);
    if (character != m_impl->entityToCharacter.end())
    {
        character->second->SetLinearVelocity(JPH::Vec3(velocity.x, velocity.y, velocity.z));
        return true;
    }

    auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
    const BodyTarget target = bodyTarget(m_impl->entityToBody, bodyInterface, entityId);
    if (!target.movable)
    {
        HE_LOG_THROTTLE(Physics, Warning, 5.0, "setVelocity on entity %u did nothing: %s",
                        entityId,
                        target.exists ? "its rigid body is Static, which never moves"
                                      : kNoBodyReason);
        return false;
    }
    // Jolt wakes the body here as long as the velocity is not near zero, which
    // is the rule we want: a body told to stand still may stay asleep, a
    // sleeping crate handed a real velocity starts moving.
    bodyInterface.SetLinearVelocity(target.id, JPH::Vec3(velocity.x, velocity.y, velocity.z));
    return true;
}

glm::vec3 PhysicsWorld::getVelocity(uint32_t entityId) const
{
    if (!m_impl) return glm::vec3(0.0f);

    const auto character = m_impl->entityToCharacter.find(entityId);
    if (character != m_impl->entityToCharacter.end())
    {
        const JPH::Vec3 v = character->second->GetLinearVelocity();
        return { v.GetX(), v.GetY(), v.GetZ() };
    }

    const auto body = m_impl->entityToBody.find(entityId);
    if (body == m_impl->entityToBody.end())
    {
        HE_LOG_THROTTLE(Physics, Warning, 5.0, "getVelocity on entity %u reads zero: %s",
                        entityId, kNoBodyReason);
        return glm::vec3(0.0f);
    }
    // A static body answers zero without complaint: that is its true velocity,
    // not a lookup that failed.
    const JPH::Vec3 v = m_impl->physicsSystem.GetBodyInterface().GetLinearVelocity(body->second);
    return { v.GetX(), v.GetY(), v.GetZ() };
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

bool PhysicsWorld::jumpCharacter(uint32_t entityId)
{
    if (!m_impl)
        return false;

    // The authored speed, read at the moment of the jump: retuning jumpSpeed
    // during play then lands on the next jump instead of on the next scene load.
    const CharacterControllerComponent* cc = characterComponentOf(m_impl->world, entityId);
    if (!cc)
    {
        HE_LOG_THROTTLE(Physics, Warning, 5.0,
                        "locomotion.jump on entity %u did nothing: %s",
                        entityId, kNoCharacterReason);
        return false;
    }
    return jumpCharacter(entityId, cc->jumpSpeed);
}

bool PhysicsWorld::jumpCharacter(uint32_t entityId, float speed)
{
    if (!m_impl)
        return false;

    const auto characterIt = m_impl->entityToCharacter.find(entityId);
    if (characterIt == m_impl->entityToCharacter.end())
    {
        HE_LOG_THROTTLE(Physics, Warning, 5.0,
                        "locomotion.jump on entity %u did nothing: %s",
                        entityId, kNoCharacterReason);
        return false;
    }

    // The component is where the ground answer and the velocity mirror live, so
    // without it there is no jump to speak of — see the header.
    CharacterControllerComponent* cc = characterComponentOf(m_impl->world, entityId);
    if (!cc)
    {
        HE_LOG_THROTTLE(Physics, Warning, 5.0,
                        "locomotion.jump on entity %u did nothing: %s",
                        entityId, kNoCharacterReason);
        return false;
    }

    // Written as !(> 0) so a NaN speed is refused here rather than poisoning the
    // character's velocity for the rest of the session.
    if (!(speed > 0.0f))
    {
        HE_LOG_THROTTLE(Physics, Warning, 5.0,
                        "locomotion.jump on entity %u did nothing: a jump speed of %.2f m/s "
                        "is not upward — give CharacterControllerComponent::jumpSpeed (or "
                        "the speed passed to locomotion.jumpWith) a positive value",
                        entityId, static_cast<double>(speed));
        return false;
    }

    // The gate, from the component rather than from Jolt's live ground state, so
    // that the answer a script gets from movement.isGrounded is literally the one
    // used here. Plus the coyote grace, which is only reachable while airborne.
    //
    // A refusal in mid-air is NOT logged: it is the normal answer to a jump
    // button pressed while falling, and a held button would ask sixty times a
    // second.
    if (!cc->isGrounded && cc->airTime >= kCoyoteWindow)
        return false;

    // REPLACE the vertical velocity, never add to it: a jump has one height, and
    // adding would make it depend on whether the character happened to be rising
    // off a ramp or already sinking. Horizontal motion is untouched, so a running
    // jump keeps its run.
    //
    // Nothing here undoes the jump before it is simulated: step() only subtracts
    // gravity from a character that is NOT supported, and this one still is, so
    // the first step gets the full speed. Jolt's ExtendedUpdate leaves the
    // velocity alone (it moves a copy) and its stick-to-floor pass is guarded by
    // "not moving up", so it will not pull the character back down either.
    JPH::CharacterVirtual& character = *characterIt->second;
    JPH::Vec3              vel       = character.GetLinearVelocity();
    vel.SetY(speed);
    character.SetLinearVelocity(vel);

    // The mirror, and the credit. isGrounded goes false a step early on purpose:
    // it is about to be true anyway, it lets the same frame's animation react,
    // and it closes the double-fire a frame that owes no physics step would
    // otherwise open (jump twice, hear the sound twice, rise once). The one cost
    // is a frame of "airborne" for a jump into a low ceiling, which the next step
    // corrects.
    cc->velocity   = { vel.GetX(), vel.GetY(), vel.GetZ() };
    cc->isGrounded = false;
    cc->airTime    = kCoyoteWindow;

    // The kinematic collision proxy needs nothing from here: step() drags it to
    // wherever the character ends up, so it follows the jump by construction.
    return true;
}

void PhysicsWorld::setGravity(const glm::vec3& gravity)
{
    if (!m_impl) return;
    m_impl->physicsSystem.SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));

    // Wake everything that had settled. Gravity is read per step from the
    // system, but a sleeping body is not stepped at all — so without this,
    // flipping gravity leaves every crate that had come to rest hanging in the
    // air until something else happens to touch it.
    auto& bodyInterface = m_impl->physicsSystem.GetBodyInterface();
    for (const auto& entry : m_impl->entityToBody)
        if (bodyInterface.GetMotionType(entry.second) != JPH::EMotionType::Static)
            bodyInterface.ActivateBody(entry.second);
}

glm::vec3 PhysicsWorld::gravity() const
{
    if (!m_impl) return glm::vec3(0.0f);
    const JPH::Vec3 g = m_impl->physicsSystem.GetGravity();
    return { g.GetX(), g.GetY(), g.GetZ() };
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

    // Same Remove-then-Destroy pair removeEntity() uses, in bulk. The per-entity
    // contact purge is skipped because reset() below drops the whole table at
    // once — one pass instead of one scan per body.
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
    // The world pointer goes too: clear() is what runs when a scene is torn down
    // or swapped, and a stale HorizonWorld* is the classic dangling read.
    m_impl->world = nullptr;
}
