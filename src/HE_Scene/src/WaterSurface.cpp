#include "HorizonScene/WaterSurface.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/WaterMesh.h"
#include "HorizonScene/WaterSpray.h"
#include "HorizonScene/Components/ParticleSystemComponent.h"
#include "HorizonScene/WaterField.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/WaterSurfaceComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <Diagnostics/Log.h>
#include <Diagnostics/Profiler.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <Renderer/IRenderer.h>
#include <entt/entt.hpp>

#include <algorithm>
#include <map>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using namespace HE::water;

    WaterSurface::Stats g_stats;
    // The Field::revision the spray emitters of a landscape were last made for (per world and landscape).
    std::map<std::pair<const HorizonWorld*, uint32_t>, uint64_t> g_sprayRevision;

    // Surface entity → the mesh it owns, per world (entity ids are per registry).
    // The component carries the same UUID; this copy is what lets a mesh be given
    // back after its entity is gone, and then the component is no longer there to
    // ask. Entt recycles handles, so the key alone is not proof — the UUID must
    // match too (the RopeTrailSystem / terrain tessellation bookkeeping).
    std::unordered_map<const HorizonWorld*, std::unordered_map<uint32_t, HE::UUID>> g_owned;

    bool terrainHasArea(const TerrainComponent& tc)
    {
        return std::isfinite(tc.sizeX) && std::isfinite(tc.sizeZ) && tc.sizeX > 0.0f && tc.sizeZ > 0.0f;
    }

    bool overlap(const CellRect& a, const CellRect& b)
    {
        return a.valid() && b.valid() &&
               a.x0 <= b.x1 && a.x1 >= b.x0 && a.z0 <= b.z1 && a.z1 >= b.z0;
    }

    uint64_t mix(uint64_t h, const void* data, size_t bytes)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }
    template <typename T> uint64_t mix(uint64_t h, const T& v) { return mix(h, &v, sizeof(T)); }

    // Everything that is the same for all bodies of a landscape and moves their
    // mesh without moving a cell (the shore clipping settings included).
    uint64_t paramsKeyOf(const TerrainComponent& tc, const glm::vec2& uvOrigin, float shoreTiltScale,
                         const glm::vec4& streamTurn)
    {
        uint64_t h = 1469598103934665603ull;
        h = mix(h, tc.sizeX); h = mix(h, tc.sizeZ); h = mix(h, tc.water.res);
        h = mix(h, uvOrigin.x); h = mix(h, uvOrigin.y);
        h = mix(h, WaterSurface::kUvMetersPerTile);
        h = mix(h, tc.water.clipToGround); h = mix(h, tc.water.shoreOvershoot);
        h = mix(h, shoreTiltScale);
        h = mix(h, streamTurn.x); h = mix(h, streamTurn.y); h = mix(h, streamTurn.z); h = mix(h, streamTurn.w);
        return h;
    }

    // sy/sx of the landscape's own world scale (Thema 206 Schritt 8): the water
    // material's vertex stage always reads the normal through mat3(uModel) (every
    // backend, MaterialShaderLibrary.cpp), never an inverse-transpose normal matrix, so
    // a non-uniform X-vs-Y world scale on the landscape (or one of ITS ancestors) scales
    // the baked shore tilt by sx/sy before the shader sees it. Baking in the inverse
    // ratio cancels that; a rotation needs no such term (SurfaceMesh's tilt survives any
    // heading, see MatGen/main.cpp shoreE). Column length, not t.scale: a parent chain's
    // combined rotation+scale is exactly what mat3(uModel) multiplies, and TRS composition
    // (T * R * S per node) keeps each column's length equal to that node's own scale
    // factor regardless of rotation — this only breaks under shear, which this engine's
    // transform system does not otherwise produce.
    float shoreTiltScaleOf(HorizonWorld& world, entt::entity te)
    {
        const glm::mat3 m(HE::worldMatrixOf(world, te));
        const float sx = glm::length(m[0]), sy = glm::length(m[1]);
        return sy / std::max(sx, 1e-6f);
    }

    // The landscape's turn about Y without its scale, for a stream's slope vector: the world x and z of
    // the local x axis, then of the local z axis (HE::worldMatrixOf's columns, the way WaterQuery turns a
    // current into world space).
    glm::vec4 streamTurnOf(HorizonWorld& world, entt::entity te)
    {
        const glm::mat3 m(HE::worldMatrixOf(world, te));
        const auto axis = [&](int c) {
            const float len = glm::length(m[c]);
            return len > 1.0e-12f ? m[c] / len : glm::vec3(c == 0, c == 1, c == 2);
        };
        const glm::vec3 ax = axis(0), az = axis(2);
        return glm::vec4(ax.x, ax.z, az.x, az.z);
    }

    // The mesh asset behind a surface goes with it: out of the renderer's cache,
    // out of the content manager.
    void giveBackMesh(ContentManager& cm, IRenderer* renderer, const HE::UUID& id)
    {
        if (id == HE::UUID{}) return;
        if (renderer) renderer->InvalidateMesh(id);
        if (!cm.unloadAsset(id))
            HE_LOG_DEBUG(Asset, "Water surface mesh %016llx%016llx stayed loaded"
                         " — something still holds a handle on it",
                         static_cast<unsigned long long>(id.hi),
                         static_cast<unsigned long long>(id.lo));
    }

    void removeSurface(HorizonWorld& world, ContentManager& cm, IRenderer* renderer, entt::entity e)
    {
        auto& reg = world.registry();
        HE::UUID mesh{};
        if (const auto* ws = reg.try_get<WaterSurfaceComponent>(e)) mesh = ws->meshId;
        if (auto w = g_owned.find(&world); w != g_owned.end())
            w->second.erase(static_cast<uint32_t>(e));
        world.destroyEntity(e);
        giveBackMesh(cm, renderer, mesh);
    }

    // The material a water body draws with: the landscape's water material (Field::material, the engine's Water by
    // default), or the same material as a Material Instance with Static Switches set. "Tessellation" (a landscape's
    // setting): the swell as real geometry. "Flow" (a body with a current, Thema 232 step 3): the ripples, foam and
    // caustics carried along it; both at once have an instance of their own. "Stream" (Body::stream) is Flow plus the
    // per-vertex current and slope of a stream; it is never tessellated. The instance is made here, in memory, on
    // first use; a material without the switches gives an instance that draws like itself. A build that cannot make
    // it (a packed game whose water has no graph to regenerate from, no shader compiler) falls back, once and for
    // good, to the next simpler variant (stream -> flow -> plain, flow + tessellation -> tessellation): a surface
    // must never turn into the grey fallback. No switch asked for: the material itself, nothing made.
    //
    // A variant's UUID is its base's with lo + 1..4 (tessellation 1, flow 2, flow + tessellation 3, stream 4): for the
    // engine's Water {0x600, 1} those are the ids DefaultAssets.h names (kEngineWaterTessellatedMaterialId …).
    HE::UUID waterVariantId(const HE::UUID& base, bool tessellation, bool flow, bool stream)
    {
        const uint64_t k = stream ? 4u : flow ? (tessellation ? 3u : 2u) : 1u;
        return HE::UUID{ base.hi, base.lo + k };
    }

    HE::UUID waterMaterialFor(ContentManager& cm, const HE::UUID& base, bool tessellation, bool flow, bool stream = false)
    {
        if (stream) flow = true;
        if (stream) tessellation = false;
        if (base == HE::UUID{} || (!tessellation && !flow)) return base;
        const HE::UUID id = waterVariantId(base, tessellation, flow, stream);
        static std::set<std::pair<const ContentManager*, std::pair<uint64_t, uint64_t>>> failed;
        const auto simpler = [&] { return waterMaterialFor(cm, base, tessellation, false, false); };
        const auto simplerStream = [&] { return waterMaterialFor(cm, base, false, true, false); };
        const auto fallback = [&] { return stream ? simplerStream() : flow ? simpler() : base; };
        if (failed.count({ &cm, { id.hi, id.lo } })) return fallback();
        auto ready = [&] {
            const MaterialAsset* m = cm.getMaterial(id);
            return m && !m->customShaderFragGlsl.empty();
        };
        if (ready()) return id;
        const MaterialAsset* parent = cm.getMaterial(base);
        if (!parent) return fallback();   // parent not here yet: retry next tick
        if (!cm.getMaterial(id))
        {
            MaterialAsset a;
            a.id   = id;
            const char* suffix = stream ? "Stream" : flow ? (tessellation ? "FlowTessellated" : "Flow") : "Tessellated";
            a.name = parent->name + suffix;
            a.path = std::string("mem://") + parent->path + "#" + suffix;
            a.parentMaterialPath = parent->path;
            if (tessellation) { a.instanceSwitchNames.push_back("Tessellation"); a.instanceSwitchValues.push_back(1); }
            if (flow)         { a.instanceSwitchNames.push_back("Flow");         a.instanceSwitchValues.push_back(1); }
            if (stream)       { a.instanceSwitchNames.push_back("Stream");       a.instanceSwitchValues.push_back(1); }
            cm.registerMaterial(std::move(a));
        }
        cm.syncMaterialInstance(id);
        if (ready()) return id;
        failed.insert({ &cm, { id.hi, id.lo } });
        HE_LOG_WARN(Asset, "Water surface: %s needs the water material's graph and a shader "
                           "compiler; drawing without it", stream ? "a stream" : flow ? "a flowing body" : "tessellation");
        return fallback();
    }

    // A knob of the water material on the surface entity's own MaterialComponent, as the per-entity
    // override (the instance carries only the switches; the numbers are per body: a river and a sea
    // on one landscape differ). `v` null removes it again, so the plain water carries nothing extra.
    void setKnob(MaterialComponent& mat, const char* knob, const float* v)
    {
        auto it = std::find_if(mat.paramOverrides.begin(), mat.paramOverrides.end(),
                               [knob](const MaterialParamOverride& o) { return o.name == knob; });
        if (!v)
        {
            if (it != mat.paramOverrides.end()) { mat.paramOverrides.erase(it); mat.dirty = true; }
            return;
        }
        if (it == mat.paramOverrides.end())
        {
            MaterialParamOverride o;
            o.name = knob;
            mat.paramOverrides.push_back(o);
            it = mat.paramOverrides.end() - 1;
        }
        if (std::equal(v, v + 4, it->value)) return;
        std::copy(v, v + 4, it->value);
        mat.dirty = true;
    }

    // The tessellated surface's numbers for a body: the material's "TessWaves" knob. Off or no body:
    // the override is removed again.
    void applyTessParams(MaterialComponent& mat, const Field& f, uint16_t bodyId)
    {
        const Body* body = f.tessellation ? f.findBody(bodyId) : nullptr;
        const float v[4] = { body ? body->tessellation.height : 0.0f, body ? body->tessellation.shoreWidth : 0.0f, 0.0f, 0.0f };
        setKnob(mat, "TessWaves", body ? v : nullptr);
    }

    // The body's current in WORLD axes (m/s, x and z): BodyPhysics::flow is terrain-local, the material
    // reads world positions. A landscape's world rotation (HE::worldMatrixOf's columns without their
    // scale, the way WaterQuery turns it) moves it; a body without a current, or none at all, is zero.
    glm::vec2 worldFlowFrom(HorizonWorld& world, entt::entity te, const glm::vec2& lf)
    {
        if (!(lf.x * lf.x + lf.y * lf.y > WaterSurface::kMinVisibleFlow * WaterSurface::kMinVisibleFlow)) return glm::vec2(0.0f);   // NaN too
        const glm::mat3 m(HE::worldMatrixOf(world, te));
        const auto axis = [&](int c) {
            const float len = glm::length(m[c]);
            return len > 1.0e-12f ? m[c] / len : glm::vec3(c == 0, c == 1, c == 2);
        };
        const glm::vec3 w = axis(0) * lf.x + axis(2) * lf.y;
        return glm::vec2(w.x, w.z);
    }

    glm::vec2 worldFlowOf(HorizonWorld& world, entt::entity te, const Field& f, uint16_t bodyId)
    {
        const Body* body = f.findBody(bodyId);
        return body ? worldFlowFrom(world, te, body->physics.flow) : glm::vec2(0.0f);
    }

    // The flowing body's "Flow" knob (world x, z). Zero: the knob goes away (and with it the instance
    // that reads it, see waterMaterialFor). z, w: the deepest water under the mesh and "z is known"
    // (WaterSurfaceComponent::deepest, -1 = unknown): a camera further below the surface than that is
    // not in this water, and the material shows it the top side instead of the underside (Thema 232
    // Schritt 13: a camera beside a stream on a hill is under the plane of every pool above it).
    // `stream`: the body is a stream, whose current is in the mesh (UV), not here: the knob carries only the
    // depth, and is always there (the Stream variant reads it).
    void applyFlowParams(MaterialComponent& mat, const glm::vec2& worldFlow, float deepest, bool stream = false)
    {
        const bool flowing = stream || worldFlow.x != 0.0f || worldFlow.y != 0.0f;
        const bool known   = deepest >= 0.0f;
        const float v[4] = { worldFlow.x, worldFlow.y, known ? deepest : 0.0f, known ? 1.0f : 0.0f };
        setKnob(mat, "Flow", flowing ? v : nullptr);
    }

    // Material first: with it not resident the renderer would draw the surface
    // with the plain fallback, which is a grey slab and looks like a bug in the
    // mesh. The landscape's water material is an ordinary reference: made resident
    // by its UUID like any other. One that cannot be (not in the content, a unit
    // test without EngineContent) is said once.
    void ensureWaterMaterial(ContentManager& cm, const HE::UUID& material)
    {
        if (material == HE::UUID{} || cm.ensureResident(material)) return;
        static bool said = false;
        if (said) return;
        said = true;
        HE_LOG_WARN(Asset, "Water surface: the landscape's water material %016llx%016llx is not available — "
                           "water will draw with the fallback material",
                    static_cast<unsigned long long>(material.hi), static_cast<unsigned long long>(material.lo));
    }

    // Per landscape.
    struct Pass
    {
        HorizonWorld&   world;
        ContentManager& cm;
        IRenderer*      renderer;

        entt::registry& reg() { return world.registry(); }

        void apply(entt::entity te, uint16_t bodyId, entt::entity existing,
                   SurfaceMesh& s, uint64_t paramsKey, WaterSurface::Stats& stats, bool tessellation)
        {
            auto& r = reg();
            HE::UUID meshId{};
            bool reuseMesh = false;
            if (existing != entt::null)
                if (const auto* ws = r.try_get<WaterSurfaceComponent>(existing);
                    ws && ws->meshId != HE::UUID{} && cm.getStaticMesh(ws->meshId) != nullptr)
                {
                    meshId = ws->meshId;
                    reuseMesh = true;
                }

            if (reuseMesh)
            {
                cm.replaceStaticMesh(meshId, std::move(s.mesh));
                if (renderer) renderer->InvalidateMesh(meshId);
            }
            else
                meshId = cm.registerStaticMesh(std::move(s.mesh));

            entt::entity e = existing;
            if (e == entt::null)
            {
                const Field& fld = reg().get<TerrainComponent>(te).water;
                ensureWaterMaterial(cm, fld.material);
                e = world.createEntity("WaterSurface");
                world.reparentEntity(e, te);
                MaterialComponent mat;
                // A stream draws with the stream material (never tessellated: a film on the ground); any other
                // body with a current with the flow one.
                const Body* bd = fld.findBody(bodyId);
                const bool isStream = bd && bd->stream;
                const glm::vec2 flow = isStream ? glm::vec2(0.0f) : worldFlowOf(world, te, fld, bodyId);
                mat.materialAssetId = waterMaterialFor(cm, fld.material, tessellation, flow != glm::vec2(0.0f), isStream);
                if (!isStream) applyTessParams(mat, fld, bodyId);
                applyFlowParams(mat, flow, s.deepest, isStream);
                r.emplace_or_replace<MaterialComponent>(e, mat);
                ++stats.created;
            }
            else if (auto* mat = r.try_get<MaterialComponent>(e))
            {
                // A rebuilt sheet can have new depths (a stroke went on, the ground was dug): the knob now,
                // not on the next tick.
                const Field& fld = reg().get<TerrainComponent>(te).water;
                const Body* bd = fld.findBody(bodyId);
                const bool isStream = bd && bd->stream;
                applyFlowParams(*mat, isStream ? glm::vec2(0.0f) : worldFlowOf(world, te, fld, bodyId), s.deepest, isStream);
            }

            WaterSurfaceComponent ws;
            ws.terrain   = te;
            ws.body      = bodyId;
            ws.meshId    = meshId;
            ws.hash      = s.hash;
            ws.paramsKey = paramsKey;
            ws.level     = s.level;
            ws.cellX0 = s.cells.x0; ws.cellZ0 = s.cells.z0;
            ws.cellX1 = s.cells.x1; ws.cellZ1 = s.cells.z1;
            ws.triangles = s.triangles;
            ws.deepest   = s.deepest;
            r.emplace_or_replace<WaterSurfaceComponent>(e, ws);

            // The entity stands at the middle of the body's box, at the surface's
            // height; the vertices are relative to that point.
            TransformComponent tf;
            if (const auto* old = r.try_get<TransformComponent>(e)) tf = *old;
            tf.position = glm::vec3(s.center.x, s.level, s.center.y);
            tf.dirty = true;
            r.emplace_or_replace<TransformComponent>(e, tf);

            MeshComponent mc;
            mc.meshAssetId    = meshId;
            mc.castsShadow    = false;     // a translucent sheet; its shadow would be a hard grey slab
            mc.dirty          = true;
            r.emplace_or_replace<MeshComponent>(e, mc);

            g_owned[&world][static_cast<uint32_t>(e)] = meshId;
            ++stats.rebuilt;
            stats.triangles += s.triangles;
        }


        // ── Spray (WaterSpray.h) ─────────────────────────────────────────────────────
        // At every fall of the landscape's streams: the curtain from its lip, the drops and the mist at its foot. Each is
        // a generated child of the landscape (never saved) with a ParticleSystemComponent that plays the particle graph
        // the landscape's water names (Field::fallCurtain/fallDrops/fallMist, the engine's by default — fx_gen) and
        // tunes it per fall with ParticleEmitterModifiers: how much and how wide (the fall's strength, the edge's
        // width), how long (the fall's time), which way (the water's). The LOOK is the asset's — size, colour, fade,
        // billboard, material, in its graph, at strength 0 and a fall of 1 s. Found again when the water or the
        // ground under it changed (Field::revision), otherwise left alone.
        struct SprayItem { uint64_t key; glm::vec3 pos; float strength; HE::UUID asset; ParticleEmitterModifiers mod; };

        // A foot is as wide as the water that lands on it (SprayFoot::halfWidth): the graphs' strips are one 1.5 m piece
        // of edge (half 0.75 across), their rates per such piece, so a wide fall gets a wide foot of the same density.
        static float footPieces(const SprayFoot& foot) { return std::max(foot.halfWidth, 0.75f) / 0.75f; }

        // The drops at the foot: a bigger fall throws more and a little higher and further, but they stay down in the
        // mist over the foot (a metre at most), not flung across the landscape.
        static ParticleEmitterModifiers footMods(const SprayFoot& foot, const glm::vec2& worldDir)
        {
            const float s = foot.strength, w = footPieces(foot);
            ParticleEmitterModifiers m;
            m.heading       = worldDir;
            m.rateScale     = (1.0f + 3.0f * s) * w;
            m.lifetimeScale = 1.0f + 0.5f * s;
            m.sizeScale     = 1.0f + 0.8f * s;
            m.velocityScale = glm::vec3(1.0f + 2.0f * s, 1.0f + 1.2f * s, 1.0f);   // on, up, across
            m.spreadScale   = 1.0f + 0.3f * s;
            m.spawnScale    = glm::vec2(w, 1.0f + 0.5f * s);                        // across: the fall's width
            return m;
        }

        // The mist over the foot: thicker, bigger, longer for a bigger fall, along the whole width it lands on.
        static ParticleEmitterModifiers mistMods(const SprayFoot& foot, const glm::vec2& worldDir)
        {
            const float s = foot.strength, w = footPieces(foot);
            ParticleEmitterModifiers m;
            m.heading       = worldDir;
            m.rateScale     = (1.0f + 2.5f * s) * w;
            m.lifetimeScale = 1.0f + 0.5f * s;
            m.sizeScale     = 1.0f + 1.3f * s;
            m.alphaScale    = 1.0f + 2.0f * s;
            m.velocityScale = glm::vec3(1.0f + 2.0f * s, 1.0f + 1.5f * s, 1.0f);
            m.spawnScale    = glm::vec2(w, 1.0f + 0.5f * s);
            return m;
        }

        // The curtain from the whole edge at once (FallLine: one straight piece of a face's edge), as wide as the edge,
        // falling for as long as the fall is high (t = sqrt(2 h / g)), and thrown on along the current just fast enough
        // to land where the line says it lands (FallLine::run) — where this fall's foot is. As dense as the water is free:
        // off a cliff (kFallFreeSlope) all of it falls through the air, down a rounded wall most of it rides the face
        // (the stream's own sheet draws that) and the curtain is the part that comes away.
        static ParticleEmitterModifiers curtainMods(const FallLine& line, const glm::vec2& worldDir)
        {
            const float s    = line.strength;
            const float fall = std::sqrt(2.0f * std::max(line.height, 0.5f) / 9.8f);
            const float half = std::max(line.halfWidth, 0.375f);
            const float free = std::clamp((line.slope - kFallSlope) / (kFallFreeSlope - kFallSlope), 0.3f, 1.0f);
            ParticleEmitterModifiers m;
            m.heading       = worldDir;
            m.rateScale     = (1.0f + 2.6f * s) * (2.0f * half / 1.5f) * free;   // the graph's rate is per 1.5 m of edge
            m.alphaScale    = 0.6f + 0.4f * free;
            m.lifetimeScale = fall;
            m.sizeScale     = 1.0f + 0.36f * s;
            m.velocityScale = glm::vec3(std::max(line.run / fall, 0.5f), 1.0f, 1.0f);   // the graph leaves at 1 m/s
            m.spawnScale    = glm::vec2(half / 0.75f, 1.0f);   // the graph's strip is 1.5 m of edge
            return m;
        }

        // All the spray of one landscape lives within this many particles: a long cliff with dozens of lips scales every
        // emitter down together rather than simulating and drawing a hundred thousand drops.
        static constexpr float kSprayParticleBudget = 30000.0f;

        // How many particles an emitter of `asset` under `mod` keeps alive at most: its graph's own pool, scaled.
        int sprayPool(const HE::UUID& asset, const ParticleEmitterModifiers& mod)
        {
            static std::unordered_map<uint64_t, int> s_base;   // per asset (hi ^ lo): its graph's max particles
            const uint64_t k = asset.hi * 0x9E3779B97F4A7C15ull ^ asset.lo;
            auto it = s_base.find(k);
            if (it == s_base.end())
            {
                int base = HE::ParticleEmitterConfig{}.maxParticles;
                cm.ensureResident(asset);
                if (const ParticleGraphAsset* a = cm.getParticleGraph(asset))
                {
                    HE::ParticleGraph g;
                    if (HE::particleGraphFromJson(a->nodeGraphJson, g))
                    {
                        std::mt19937 rng(1);
                        base = HE::evaluateParticleGraph(g, rng).maxParticles;
                    }
                    it = s_base.emplace(k, base).first;
                }
                else
                    return applyEmitterModifiers(HE::ParticleEmitterConfig{}, mod).maxParticles;
            }
            HE::ParticleEmitterConfig c;
            c.maxParticles = it->second;
            return applyEmitterModifiers(c, mod).maxParticles;
        }

        void sprays(entt::entity te, TerrainComponent& tc, std::vector<entt::entity>& mine, WaterSurface::Stats& stats,
                    uint64_t& lastRevision)
        {
            Field& f = tc.water;
            const bool haveStream = std::any_of(f.bodies.begin(), f.bodies.end(), [](const Body& b) { return b.stream; });
            std::unordered_map<uint64_t, entt::entity> byKey;
            for (entt::entity e : mine) byKey[reg().get<WaterSurfaceComponent>(e).sprayKey] = e;
            if (!haveStream || !f.allocated() || !terrainHasArea(tc))
            {
                for (auto& kv : byKey) { removeSurface(world, cm, renderer, kv.second); ++stats.removed; }
                lastRevision = 0;
                return;
            }
            if (lastRevision == f.revision && !(byKey.empty() && f.dirty)) return;
            lastRevision = f.revision;
            HE_PROFILE_SCOPE_N("WaterSurface::sprays");   // only when the water changed: finding the falls and feet

            const glm::vec4 turn = streamTurnOf(world, te);          // local x axis -> world (x, z), local z axis -> world (x, z)
            auto toWorld = [&](const glm::vec2& d) { return glm::vec2(turn.x * d.x + turn.z * d.y, turn.y * d.x + turn.w * d.y); };
            auto keyOf = [](const glm::vec2& p, uint64_t salt) {
                return (static_cast<uint64_t>(static_cast<int64_t>(std::floor(p.x / 1.5f))) * 1000003ull ^
                        static_cast<uint64_t>(static_cast<int64_t>(std::floor(p.y / 1.5f)))) ^ salt;
            };

            // Each fall is one piece: its curtain and the foot that curtain lands on come from the same FallLine, so
            // they are as wide as each other and the spray rises where the water comes down. Then the run-outs: fast
            // water meeting calm water where nothing falls.
            std::vector<SprayItem> items;
            auto addFoot = [&](const SprayFoot& foot) {
                if (f.fallDrops != HE::UUID{})
                    items.push_back({ keyOf(foot.pos, 0x51ull), glm::vec3(foot.pos.x, foot.surfaceY, foot.pos.y), foot.strength,
                                      f.fallDrops, footMods(foot, toWorld(foot.dir)) });
                if (f.fallMist != HE::UUID{})
                    items.push_back({ keyOf(foot.pos, 0x52ull), glm::vec3(foot.pos.x, foot.surfaceY + 0.3f, foot.pos.y), foot.strength,
                                      f.fallMist, mistMods(foot, toWorld(foot.dir)) });
            };
            const std::vector<Fall> falls = findFalls(tc);
            for (const Fall& fall : falls)
            {
                const FallLine& line = fall.line;
                if (f.fallCurtain != HE::UUID{})
                    items.push_back({ keyOf(line.center, 0xA7ull << 32), glm::vec3(line.center.x, line.surfaceY, line.center.y),
                                      line.strength, f.fallCurtain, curtainMods(line, toWorld(line.dir)) });
                addFoot(fall.foot);
            }
            for (const SprayFoot& foot : findRunoutFeet(tc, falls)) addFoot(foot);
            float alive = 0.0f;
            for (const SprayItem& it : items) alive += static_cast<float>(sprayPool(it.asset, it.mod));
            if (alive > kSprayParticleBudget)
                for (SprayItem& it : items) it.mod.rateScale *= kSprayParticleBudget / alive;   // the pool shrinks with it

            for (const SprayItem& it : items)
            {
                entt::entity e = entt::null;
                if (const auto f2 = byKey.find(it.key); f2 != byKey.end()) { e = f2->second; byKey.erase(f2); }
                if (e == entt::null)
                {
                    e = world.createEntity("WaterSpray");
                    world.reparentEntity(e, te);
                    WaterSurfaceComponent ws;
                    ws.terrain = te; ws.spray = true; ws.sprayKey = it.key; ws.sprayStrength = it.strength;
                    r_emplace(e, ws);
                    TransformComponent tf;
                    tf.position = it.pos;
                    tf.dirty = true;
                    r_emplace(e, tf);
                    ParticleSystemComponent ps;
                    ps.playing         = true;
                    ps.particleAssetId = it.asset;
                    ps.modifiers       = it.mod;
                    ps.configDirty     = true;   // resolved from the asset (and the modifiers) on the next particle tick
                    r_emplace(e, ps);
                    ++stats.created;
                    HE_LOG_DEBUG(Particle, "Water spray: an emitter at (%.1f, %.1f, %.1f), strength %.2f", it.pos.x, it.pos.y, it.pos.z, it.strength);
                    continue;
                }
                auto& ws = reg().get<WaterSurfaceComponent>(e);
                // The water changed (that is all that brings us here): the fall may be wider, the foot stronger, the
                // landscape may name another particle graph.
                if (auto* ps = reg().try_get<ParticleSystemComponent>(e);
                    ps && (ps->particleAssetId != it.asset || !(ps->modifiers == it.mod)))
                {
                    ps->particleAssetId = it.asset;
                    ps->modifiers       = it.mod;
                    ps->configDirty     = true;
                }
                ws.sprayStrength = it.strength;
                auto& tf = reg().get_or_emplace<TransformComponent>(e);
                if (tf.position != it.pos) { tf.position = it.pos; tf.dirty = true; }
            }
            // An emitter whose foot or lip is no more.
            for (auto& kv : byKey) { removeSurface(world, cm, renderer, kv.second); ++stats.removed; }
        }

        template <class T> void r_emplace(entt::entity e, const T& v) { reg().template emplace_or_replace<T>(e, v); }

        void terrain(entt::entity te, TerrainComponent& tc, std::vector<entt::entity>& surfaces,
                     WaterSurface::Stats& stats)
        {
            Field& f = tc.water;

            // One surface per body. A second one for the same body (a copied
            // entity) would draw the lake twice and fight over the mesh: keep
            // the first.
            std::unordered_map<uint16_t, entt::entity> byBody;
            for (entt::entity e : surfaces)
            {
                const auto& ws = reg().get<WaterSurfaceComponent>(e);
                if (byBody.count(ws.body)) { removeSurface(world, cm, renderer, e); ++stats.removed; continue; }
                byBody[ws.body] = e;
            }

            const bool usable = f.allocated() && terrainHasArea(tc) && !f.bodies.empty();
            if (!usable)
            {
                for (auto& kv : byBody) { removeSurface(world, cm, renderer, kv.second); ++stats.removed; }
                f.dirty = false;
                return;
            }
            ++stats.terrains;

            const glm::vec3 wp = HE::worldPositionOf(world, te);
            SurfaceOptions opt;
            opt.uvOrigin        = glm::vec2(wp.x, wp.z);
            opt.uvMetersPerTile = WaterSurface::kUvMetersPerTile;
            opt.clipToGround    = f.clipToGround;
            opt.shoreOvershoot  = f.shoreOvershoot;
            opt.shoreFade       = true;     // the engine water material softens the shore from it
            opt.shoreTiltScale  = shoreTiltScaleOf(world, te);
            opt.streamTurn      = streamTurnOf(world, te);
            const uint64_t paramsKey = paramsKeyOf(tc, opt.uvOrigin, opt.shoreTiltScale, opt.streamTurn);

            // The material stays with the surface, not only with its creation: a scene
            // that was re-opened, an unload sweep or a snapshot that did not carry the
            // component leaves a surface that lives on and draws the plain fallback.
            // One look at the residency per tick (a map lookup), a retry at most once
            // a second when the file is missing.
            if (!byBody.empty())
            {
                if (f.material != HE::UUID{} && !cm.isLoaded(f.material))
                {
                    if (f.materialWait > 0) --f.materialWait;
                    else
                    {
                        ensureWaterMaterial(cm, f.material);
                        if (!cm.isLoaded(f.material)) f.materialWait = 60;
                    }
                }
                for (auto& kv : byBody)
                {
                    // Per body: only one with a current draws with the flow variant, a stream with its own.
                    const Body* bd = f.findBody(kv.first);
                    const bool isStream = bd && bd->stream;
                    const glm::vec2 flow = isStream ? glm::vec2(0.0f) : worldFlowOf(world, te, f, kv.first);
                    const HE::UUID wanted = waterMaterialFor(cm, f.material, f.tessellation, flow != glm::vec2(0.0f), isStream);
                    auto* mat = reg().try_get<MaterialComponent>(kv.second);
                    if (!mat)
                        mat = &reg().emplace<MaterialComponent>(kv.second), mat->materialAssetId = wanted;
                    else if (mat->materialAssetId != wanted)
                    {
                        mat->materialAssetId = wanted;
                        mat->dirty = true;
                    }
                    if (isStream) setKnob(*mat, "TessWaves", nullptr);
                    else          applyTessParams(*mat, f, kv.first);
                    applyFlowParams(*mat, flow, reg().get<WaterSurfaceComponent>(kv.second).deepest, isStream);
                }
            }

            // Steady state: nothing was edited, every surface still belongs to a
            // body, was built with today's parameters and stands at its body's
            // level. That is nearly every tick, and it costs no more than this.
            bool steady = !f.dirty;
            if (steady)
                for (const auto& kv : byBody)
                {
                    const auto& ws = reg().get<WaterSurfaceComponent>(kv.second);
                    const Body* b = f.findBody(kv.first);
                    if (!b || ws.paramsKey != paramsKey || ws.level != b->level) { steady = false; break; }
                }
            if (steady) return;

            const std::vector<CellRect> extents = allBodyCells(tc);
            // The edited rectangle in cells, one cell wider all round: a contour
            // reads the cells around a corner, so a change in the cell next door
            // moves it.
            CellRect dirtyCells;
            if (f.dirty)
            {
                const glm::vec2 cs = cellSize(tc);
                dirtyCells = cellRect(tc, f.dirtyMinX - cs.x, f.dirtyMinZ - cs.y,
                                          f.dirtyMaxX + cs.x, f.dirtyMaxZ + cs.y);
            }

            // Flat neighbours at one level are ONE sheet (WaterMesh.h flatGroupLeads): the lowest id draws the group.
            const std::unordered_map<uint16_t, uint16_t> leadOf = flatGroupLeads(tc);
            std::unordered_map<uint16_t, std::vector<size_t>> membersOf;       // lead id -> the other bodies of its group (indices)
            for (size_t i = 0; i < f.bodies.size(); ++i)
                if (const auto it = leadOf.find(f.bodies[i].id); it != leadOf.end()) membersOf[it->second].push_back(i);

            for (size_t i = 0; i < f.bodies.size(); ++i)
            {
                const Body& body = f.bodies[i];
                CellRect ext = extents[i];
                std::vector<uint16_t> groupIds;
                if (leadOf.count(body.id))
                {
                    // A member of a group draws nothing of its own: its lead's sheet covers it.
                    if (const auto it = byBody.find(body.id); it != byBody.end())
                    {
                        removeSurface(world, cm, renderer, it->second);
                        ++stats.removed;
                        byBody.erase(it);
                    }
                    continue;
                }
                if (const auto m = membersOf.find(body.id); m != membersOf.end())
                {
                    groupIds.push_back(body.id);
                    for (const size_t k : m->second)
                    {
                        groupIds.push_back(f.bodies[k].id);
                        const CellRect& e2 = extents[k];
                        if (!e2.valid()) continue;
                        if (!ext.valid()) { ext = e2; continue; }
                        ext.x0 = std::min(ext.x0, e2.x0); ext.z0 = std::min(ext.z0, e2.z0);
                        ext.x1 = std::max(ext.x1, e2.x1); ext.z1 = std::max(ext.z1, e2.z1);
                    }
                }
                // Taken out of the map as it is looked at: what is still in it after
                // the loop belongs to a body that no longer exists.
                entt::entity existing = entt::null;
                if (const auto it = byBody.find(body.id); it != byBody.end())
                {
                    existing = it->second;
                    byBody.erase(it);
                }

                if (!ext.valid())                       // a body with no water draws nothing
                {
                    if (existing != entt::null) { removeSurface(world, cm, renderer, existing); ++stats.removed; }
                    continue;
                }

                bool candidate = existing == entt::null;
                if (!candidate)
                {
                    const auto& ws = reg().get<WaterSurfaceComponent>(existing);
                    CellRect built; built.x0 = ws.cellX0; built.z0 = ws.cellZ0; built.x1 = ws.cellX1; built.z1 = ws.cellZ1;
                    candidate = ws.paramsKey != paramsKey || ws.level != body.level ||
                                overlap(dirtyCells, ext) || overlap(dirtyCells, built);
                }
                if (!candidate) { ++stats.kept; continue; }

                // A stream is the ground lifted; every other body a flat sheet at its level.
                SurfaceOptions bodyOpt = opt;
                if (body.stream) bodyOpt.joinBody = body.id;     // a stream joins the water it runs into (WaterMesh.h joinReaches)
                if (body.stream)
                {
                    bodyOpt.streamDepth = body.streamDepth;
                    bodyOpt.streamBody = body.id;
                    bodyOpt.streamJoinKey = streamJoinKeyOf(f);
                }
                Lattice lattice;
                if (!(groupIds.size() > 1 ? buildLattice(tc, groupIds, ext, lattice, bodyOpt.clipFor(body.level))
                                          : buildLattice(tc, body.id, ext, lattice, bodyOpt.clipFor(body.level)))) continue;
                const uint64_t hash = surfaceHash(lattice, body.level, bodyOpt);
                if (existing != entt::null)
                {
                    auto& ws = reg().get<WaterSurfaceComponent>(existing);
                    if (ws.hash == hash)                // an edit nearby that did not reach this lake
                    {
                        ws.cellX0 = ext.x0; ws.cellZ0 = ext.z0; ws.cellX1 = ext.x1; ws.cellZ1 = ext.z1;
                        ++stats.kept;
                        continue;
                    }
                }

                SurfaceMesh s;
                if (!buildSurface(tc, lattice, ext, body.level, bodyOpt, s)) continue;
                if (s.empty())                          // wet cells but no area worth a polygon
                {
                    if (existing != entt::null) { removeSurface(world, cm, renderer, existing); ++stats.removed; }
                    continue;
                }
                apply(te, body.id, existing, s, paramsKey, stats, f.tessellation && !body.stream);
            }

            // What is left in the map belongs to a body that no longer exists.
            for (auto& kv : byBody) { removeSurface(world, cm, renderer, kv.second); ++stats.removed; }

            f.dirty = false;
        }
    };
}

namespace WaterSurface
{
    void update(HorizonWorld& world, ContentManager& cm, IRenderer* renderer)
    {
        auto& reg = world.registry();
        Stats stats;

        // ── Give back the meshes of surfaces that are gone ───────────────────
        // An undo rebuilds every generated entity; a deleted landscape takes its
        // children with it. Neither tells anyone.
        if (auto w = g_owned.find(&world); w != g_owned.end())
        {
            auto& owned = w->second;
            for (auto it = owned.begin(); it != owned.end(); )
            {
                const entt::entity e = static_cast<entt::entity>(it->first);
                const auto* ws = reg.valid(e) ? reg.try_get<WaterSurfaceComponent>(e) : nullptr;
                if (ws && ws->meshId == it->second) { ++it; continue; }
                giveBackMesh(cm, renderer, it->second);
                it = owned.erase(it);
            }
            if (owned.empty()) g_owned.erase(w);
        }

        // ── Surfaces by landscape ────────────────────────────────────────────
        std::unordered_map<uint32_t, std::vector<entt::entity>> byTerrain;
        std::unordered_map<uint32_t, std::vector<entt::entity>> spraysByTerrain;      // spray emitters (WaterSpray.h)
        std::vector<entt::entity> orphans;
        for (auto [e, ws] : reg.view<WaterSurfaceComponent>().each())
        {
            if (ws.terrain != entt::null && reg.valid(ws.terrain) && reg.all_of<TerrainComponent>(ws.terrain))
                (ws.spray ? spraysByTerrain : byTerrain)[static_cast<uint32_t>(ws.terrain)].push_back(e);
            else
                orphans.push_back(e);
        }
        for (entt::entity e : orphans) { removeSurface(world, cm, renderer, e); ++stats.removed; }

        // ── Each landscape ───────────────────────────────────────────────────
        Pass pass{ world, cm, renderer };
        std::vector<entt::entity> terrains;
        for (auto e : reg.view<TerrainComponent>()) terrains.push_back(e);   // entities are created below
        for (entt::entity te : terrains)
        {
            auto& tc = reg.get<TerrainComponent>(te);
            std::vector<entt::entity> mine;
            if (auto found = byTerrain.find(static_cast<uint32_t>(te)); found != byTerrain.end())
                mine = std::move(found->second);
            // A landscape that never had water and has none now has nothing to look at.
            std::vector<entt::entity> mySprays;
            if (auto found = spraysByTerrain.find(static_cast<uint32_t>(te)); found != spraysByTerrain.end())
                mySprays = std::move(found->second);
            if (mine.empty() && mySprays.empty() && tc.water.bodies.empty()) { tc.water.dirty = false; continue; }
            // Spray first: terrain() clears Field::dirty, which the spray pass reads.
            pass.sprays(te, tc, mySprays, stats, g_sprayRevision[{ &world, static_cast<uint32_t>(te) }]);
            pass.terrain(te, tc, mine, stats);
        }

        g_stats = stats;
    }

    const Stats& lastStats() { return g_stats; }
}
