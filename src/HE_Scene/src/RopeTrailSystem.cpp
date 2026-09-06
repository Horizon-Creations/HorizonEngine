#include "HorizonScene/RopeTrailSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/RopeComponent.h"
#include "HorizonScene/Components/TrailComponent.h"
#include <ContentManager/ContentManager.h>
#include <Renderer/IRenderer.h>
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <unordered_map>

namespace
{
    // ── Which runtime meshes are OURS ────────────────────────────────────────
    // world → (entity → the mesh UUID this system registered for it). It is the
    // ownership record, not just a cleanup list, and it answers the one question
    // a plain UUID on the component cannot: may this system REPLACE that asset?
    //
    // It may not, when the id arrived by some other route — a component copied
    // between worlds, a scene restored while the old world still holds the mesh.
    // Replacing then would overwrite somebody else's geometry, which is why an
    // unowned id is treated exactly like an empty one.
    //
    // Keyed by world because entity ids are per-registry, and file-static rather
    // than stored on the component because it is bookkeeping ABOUT the component
    // and the component is serialised. Main-thread only, like the rest of the tick.
    std::unordered_map<const HorizonWorld*, std::unordered_map<uint32_t, HE::UUID>> g_ropeMeshes;

    inline uint32_t entityKey(entt::entity e) { return static_cast<uint32_t>(e); }

    void hashBytes(uint64_t& h, const void* data, size_t bytes)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    }
    template <typename T> void hashValue(uint64_t& h, const T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>, "hash raw bytes only");
        hashBytes(h, &v, sizeof(T));
    }

    StaticMeshAsset toStaticMesh(const HE::spline::MeshData& mesh)
    {
        StaticMeshAsset a;
        a.name = "rope";
        a.path = "mem://rope";
        a.vertices = mesh.positions;
        a.normals  = mesh.normals;
        a.uvs      = mesh.uvs;
        a.indices  = mesh.indices;
        return a;
    }
}

namespace RopeTrailSystem
{

std::vector<glm::vec3> resolveControlPoints(HorizonWorld& world, entt::entity entity,
                                            const RopeComponent& rope)
{
    std::vector<glm::vec3> pts = rope.controlPoints;
    if (pts.size() < 2) return pts;

    const bool wantStart = !(rope.attachStart == HE::UUID{});
    const bool wantEnd   = !(rope.attachEnd   == HE::UUID{});
    if (!wantStart && !wantEnd) return pts;

    auto& reg = world.registry();
    // The rope's own world matrix, inverted once: an attachment is a WORLD
    // position, and the control points are local.
    const glm::mat4 toLocal = glm::inverse(HE::worldMatrixOf(world, entity));
    auto substitute = [&](const HE::UUID& id, glm::vec3& slot)
    {
        const entt::entity a = world.findByEntityId(id);
        if (a == entt::null || !reg.valid(a)) return;
        const glm::vec3 w = HE::worldPositionOf(world, a);
        slot = glm::vec3(toLocal * glm::vec4(w, 1.0f));
    };
    if (wantStart) substitute(rope.attachStart, pts.front());
    if (wantEnd)   substitute(rope.attachEnd,   pts.back());
    return pts;
}

HE::spline::MeshData buildRopeGeometry(const RopeComponent& rope,
                                       const std::vector<glm::vec3>& localPoints)
{
    using namespace HE::spline;
    if (localPoints.size() < 2) return {};

    const std::vector<glm::vec3> dense = sampleCatmullRom(localPoints, rope.samplesPerSpan);
    std::vector<glm::vec3> pts = resampleByArcLength(dense, static_cast<int>(dense.size()));

    // Sag: a parabola that is zero at both ends, straight down in the entity's
    // local space. Applied to the SAMPLES rather than the control points, so it
    // bows the whole span instead of moving the points the author placed. It
    // perturbs the even spacing slightly; a catenary solver would not, and is
    // also not what this is — see RopeComponent.
    if (std::fabs(rope.sag) > 1e-6f && pts.size() > 2)
    {
        const float last = static_cast<float>(pts.size() - 1);
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const float t = static_cast<float>(i) / last;
            pts[i].y -= rope.sag * 4.0f * t * (1.0f - t);
        }
    }

    const std::vector<Frame> frames = buildFrames(pts, glm::vec3(0.0f, 1.0f, 0.0f));
    if (frames.size() < 2) return {};

    if (rope.shape == RopeShape::Tube)
    {
        TubeParams tp;
        tp.radius         = rope.radius;
        tp.radialSegments = rope.radialSegments;
        tp.uvTileLength   = rope.uvTileLength;
        return buildTube(frames, tp);
    }

    // Ribbon: a rope-shaped band keeps a fixed orientation in the world (a strap,
    // a flag), so it takes the frame axis, not the camera. The V coordinate tiles
    // by arc length like the tube's.
    const float total = frames.back().arcLength;
    std::vector<RibbonSection> sections;
    sections.reserve(frames.size());
    for (const Frame& f : frames)
    {
        RibbonSection s;
        s.position  = f.position;
        s.halfWidth = rope.radius;
        s.v = (rope.uvTileLength > 0.0f) ? f.arcLength / rope.uvTileLength
                                         : ((total > 1e-6f) ? f.arcLength / total : 0.0f);
        sections.push_back(s);
    }
    RibbonParams rp;
    rp.cameraAligned = false;
    rp.upHint        = glm::vec3(0.0f, 1.0f, 0.0f);
    rp.twoSided      = rope.twoSidedGeometry;
    return buildRibbon(sections, rp);
}

HE::spline::MeshData buildTrailGeometry(const TrailComponent& trail, const glm::vec3& cameraPos)
{
    using namespace HE::spline;
    if (trail.points.size() < 2) return {};

    // Tip first: uv.v then runs 0 → 1 along the strip, which is the direction a
    // material graph's age gradient expects.
    std::vector<RibbonSection> sections;
    sections.reserve(trail.points.size());
    const float life = (trail.lifetime > 1e-6f) ? trail.lifetime : 0.0f;
    const float last = static_cast<float>(trail.points.size() - 1);
    for (size_t k = 0; k < trail.points.size(); ++k)
    {
        const TrailComponent::Point& p = trail.points[trail.points.size() - 1 - k];
        // Age over lifetime, so the tail sits at 1 exactly when it is about to
        // expire. Without a lifetime the position in the buffer is the only
        // ordering there is.
        const float v = (life > 0.0f) ? std::clamp(p.age / life, 0.0f, 1.0f)
                                      : (static_cast<float>(k) / last);
        RibbonSection s;
        s.position  = p.worldPos;
        s.v         = v;
        s.halfWidth = glm::mix(trail.startWidth, trail.endWidth, v) * 0.5f;
        sections.push_back(s);
    }

    RibbonParams rp;
    rp.cameraAligned = (trail.alignment == TrailAlignment::Camera);
    rp.cameraPos     = cameraPos;
    rp.twoSided      = false;   // trails are emissive by default; see docs §3.3
    return buildRibbon(sections, rp);
}

void stepTrail(TrailComponent& trail, const glm::vec3& worldPos, float dt)
{
    for (auto& p : trail.points) p.age += dt;

    // Oldest first, so expiry is a prefix — and a lifetime of zero or less means
    // "never expire by age", not "expire immediately".
    if (trail.lifetime > 0.0f)
    {
        auto alive = std::find_if(trail.points.begin(), trail.points.end(),
                                  [&](const TrailComponent::Point& p) { return p.age <= trail.lifetime; });
        trail.points.erase(trail.points.begin(), alive);
    }

    if (trail.emitting)
    {
        const float minD = std::max(0.0f, trail.minVertexDistance);
        const bool  far  = !trail.hasLastEmit
                        || glm::length(worldPos - trail.lastEmitPos) >= minD;
        if (far)
        {
            trail.points.push_back({ worldPos, 0.0f });
            trail.lastEmitPos = worldPos;
            trail.hasLastEmit = true;
        }
    }

    const size_t cap = static_cast<size_t>(std::max(2, trail.maxPoints));
    if (trail.points.size() > cap)
        trail.points.erase(trail.points.begin(),
                           trail.points.begin() + static_cast<std::ptrdiff_t>(trail.points.size() - cap));
}

uint64_t geometryHash(const RopeComponent& rope, const std::vector<glm::vec3>& localPoints)
{
    uint64_t h = 14695981039346656037ull;   // FNV-1a offset basis
    hashValue(h, static_cast<uint32_t>(localPoints.size()));
    if (!localPoints.empty())
        hashBytes(h, localPoints.data(), localPoints.size() * sizeof(glm::vec3));
    hashValue(h, static_cast<uint8_t>(rope.shape));
    hashValue(h, rope.radius);
    hashValue(h, rope.radialSegments);
    hashValue(h, rope.samplesPerSpan);
    hashValue(h, rope.uvTileLength);
    hashValue(h, rope.sag);
    hashValue(h, static_cast<uint8_t>(rope.twoSidedGeometry));
    // A hash of 0 is the "never built" marker on the component, so never return it.
    return h ? h : 1ull;
}

void update(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
            const glm::vec3& cameraPos, float dt)
{
    (void)cameraPos;   // trails are triangulated at extraction, where the camera is
    auto& reg = world.registry();

    // ── Trails: age, expire, emit ────────────────────────────────────────────
    // The emission point comes from worldPositionOf, not TransformComponent::
    // worldMatrix: nothing has propagated transforms yet this frame, and the
    // entity a trail hangs on is by definition the one that just moved.
    for (auto [e, trail] : reg.view<TrailComponent>().each())
        stepTrail(trail, HE::worldPositionOf(world, e), dt);

    // ── Ropes: rebuild only what changed ─────────────────────────────────────
    auto& owned = g_ropeMeshes[&world];
    for (auto [e, rope] : reg.view<RopeComponent>().each())
    {
        const std::vector<glm::vec3> pts = resolveControlPoints(world, e, rope);
        const uint64_t hash = geometryHash(rope, pts);

        // The MAP decides ownership, not the component. A rope's component can be
        // replaced wholesale on an entity that keeps its handle — a collaboration
        // sync and an undo both go through emplace_or_replace — and the fresh
        // component arrives with an empty runtimeMeshId. Trusting the component
        // there would register a second mesh and orphan the first: a leak, and
        // the very pool reallocation the register-once rule exists to avoid.
        const auto it = owned.find(entityKey(e));
        const bool ours = (it != owned.end()) && cm.getStaticMesh(it->second) != nullptr;
        if (ours && !(rope.runtimeMeshId == it->second))
        {
            rope.runtimeMeshId = it->second;   // hand the mesh back to its rope
            rope.builtHash     = 0;            // and rebuild: the parameters may differ
        }

        if (ours && hash == rope.builtHash) continue;

        HE::spline::MeshData mesh = buildRopeGeometry(rope, pts);
        if (mesh.empty())
        {
            // Nothing to draw (fewer than two control points). Remember the hash
            // so this is not attempted again every frame; the mesh, if there is
            // one, keeps its last contents until the rope is buildable again.
            rope.builtHash = hash;
            continue;
        }

        if (ours)
        {
            cm.replaceStaticMesh(rope.runtimeMeshId, toStaticMesh(mesh));
            if (renderer) renderer->InvalidateMesh(rope.runtimeMeshId);
        }
        else
        {
            // Register ONCE per rope. Every registration can move the asset pool
            // and invalidate every ContentManager pointer anything else holds,
            // so doing it per rebuild would be a pointer minefield, not just a leak.
            rope.runtimeMeshId  = cm.registerStaticMesh(toStaticMesh(mesh));
            owned[entityKey(e)] = rope.runtimeMeshId;
        }
        rope.builtHash = hash;
    }

    // ── Give back the meshes of ropes that are gone ──────────────────────────
    // An entt handle is recycled, so "the entity is valid" is not enough: the
    // handle may belong to a NEW rope by now, which is what the id comparison
    // catches. Done here rather than in an on_destroy listener because a listener
    // would need to hold a ContentManager pointer across the registry's own
    // teardown, and here the content manager is guaranteed to be alive.
    for (auto it = owned.begin(); it != owned.end(); )
    {
        const entt::entity e = static_cast<entt::entity>(it->first);
        const RopeComponent* rc = reg.valid(e) ? reg.try_get<RopeComponent>(e) : nullptr;
        if (rc && rc->runtimeMeshId == it->second) { ++it; continue; }

        if (!cm.unloadAsset(it->second))
            HE_LOG_DEBUG(Asset, "Rope mesh %016llx%016llx stayed loaded — something still "
                         "holds a handle on it",
                         static_cast<unsigned long long>(it->second.hi),
                         static_cast<unsigned long long>(it->second.lo));
        it = owned.erase(it);
    }
}

void releaseWorld(HorizonWorld& world, ContentManager& cm)
{
    auto found = g_ropeMeshes.find(&world);
    if (found == g_ropeMeshes.end()) return;
    for (const auto& [key, id] : found->second) cm.unloadAsset(id);
    g_ropeMeshes.erase(found);
}

}  // namespace RopeTrailSystem
