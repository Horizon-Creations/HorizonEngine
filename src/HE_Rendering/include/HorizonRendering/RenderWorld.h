#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderObject.h"
#include "CommandBuffer.h"
#include "GiLandscape.h"
#include <Renderer/UIRenderObject.h>
#include <ParticleGraph/ParticleGraph.h>
#include <Math/Math.h>
#include <JobSystem/JobSystem.h>
#include <cstdlib>
#include <memory>
#include <vector>
#include <cstdint>

struct CameraData {
    glm::mat4 view       = glm::mat4(1.0f);
    glm::mat4 projection = glm::mat4(1.0f);
    glm::vec3 position   = glm::vec3(0.0f);
};

// How far the render camera sits under a water surface this frame (Thema 185 Schritt
// 1), for the post-processing stage to read. Set by HE::queryUnderwaterCamera
// (RenderExtractor.h/.cpp) right after the camera, straight from camera.position — no
// state carried between frames, so a camera resting exactly on the waterline or
// crossing it within one frame is correct immediately, with no hysteresis.
// Thema 185 Schritt 2 adds the post pass's own two numbers, resolved here (not left
// to the shader) because they are a function of signedDepth above, which is already
// a per-frame scalar, not a per-pixel one: a camera hovering at a fixed depth sees a
// fixed tint no matter what it looks at. tintColor is shallowColor..deepColor of the
// body's UnderwaterFogParams (WaterField.h), blended by signedDepth through that
// body's own density/falloff curve. density is that same body's absorption, carried
// through unchanged for the per-pixel view-ray fog (segment length still needs the
// depth buffer, so that part stays in the shader). density is exactly 0 — the
// cue to skip the whole fog pass — when dry, EXCEPT in the waterline band just
// above a body's surface (Thema 185 Schritt 5, HE::fogWaterlineBand): there wet
// stays false but tintColor is shallowColor and density the body's, faded out over
// the band, so the pass fogs the floor the near plane bares where it slices the
// water sheet off just above the line (the shaders' per-pixel near-plane test). The
// fog gate on every backend is therefore density > 0, never wet.
struct UnderwaterCameraData {
    bool  wet         = false;  // a water surface covers the camera's XZ, AND camera.y is between that body's bed and its surface level (inclusive) — not above the surface, not under the bed
    float surfaceY    = 0.0f;   // world Y of that surface (the flat body level the material shades waves on top of; meaningful only if wet)
    float signedDepth = 0.0f;   // surfaceY - camera.y: > 0 means the camera is below the surface. 0 when !wet.
    glm::vec3 tintColor { 0.045f, 0.20f, 0.22f };   // resolved shallow→deep blend; meaningful only if density > 0
    float     density = 0.0f;                        // body's per-metre absorption; 0 when dry outside the waterline band (skip the pass)

    // Thema 185 Schritt 3 — the caustic pass's knobs, resolved here like tintColor
    // (the body's UnderwaterCausticsParams, WaterField.h). causticsIntensity already
    // carries the waterline fade-in (kCausticsFadeInDepth in RenderExtractor.cpp: 0 with
    // the camera exactly on the surface, ramping to the body's own value over the first
    // half metre of the dive), so the net never pops on at the waterline and 0 is the
    // pass's one cue to skip — dry camera, body turned it off, or camera on the line.
    float causticsIntensity = 0.0f;
    float causticsScale     = 1.0f;
    float causticsSpeed     = 1.0f;
    float causticsDepth     = 10.0f;   // depth where the net is gone (water::kDefaultCausticsDepth)
    // How far the scene's wind has turned the sea from the material's own heading,
    // radians (water::windTurnDeg). The pass turns the net's wave frame by it; the
    // wind's strength is already in causticsScale (coarser sea) and causticsSpeed.
    float causticsWindTurn  = 0.0f;

    // The tessellated surface for the post passes (UnderwaterSurfaceShared.h: heSurfaceH):
    // `surfaceHeight` 0 = the flat plane at surfaceY (a landscape without tessellation, a backend
    // that cannot do it); > 0 = the surface stands up to that multiple of the waves' own amplitude
    // above `surfaceLevel`, per pixel. surfaceY then is the surface under the CAMERA (level + wave).
    float surfaceHeight     = 0.0f;
    float surfaceShoreWidth = 1.8f;
    float surfaceLevel      = 0.0f;   // the flat level of the body (surfaceY when flat)
    float waveLengthFactor  = 1.0f;   // the wind's factors on the three trains (water::windWaveFactors)
    float waveSpeedFactor   = 1.0f;
    float waveSteepFactor   = 1.0f;
    float waveTurn          = 0.0f;   // radians (water::windTurnDeg)
};

// "no entity" for the owner ids below — 0 is a perfectly valid entt entity, so
// the sentinel has to be a value the registry never hands out.
inline constexpr uint32_t kNoOwnerEntity = 0xFFFFFFFFu;

struct LightData {
    // The entity the LightComponent sits on. Only used to keep that entity's OWN
    // mesh out of this light's shadow map: a light authored onto a mesh entity (a
    // lamp model carrying its lamp light) sits INSIDE that mesh, so the mesh fills
    // the light's depth map at z≈0 and the light shadows itself out completely —
    // the whole scene stays unlit by it. Put the light on a child entity when the
    // model really is supposed to occlude it.
    uint32_t  entityId     = kNoOwnerEntity;
    glm::vec3 position     = glm::vec3(0.0f);
    glm::vec3 direction    = glm::vec3(0.0f, -1.0f, 0.0f); // directional/spot: -Z of the light's world matrix
    glm::vec3 color        = glm::vec3(1.0f);
    float     intensity    = 1.0f;
    float     range        = 10.0f;   // point/spot attenuation radius
    float     spotAngleCos = 0.0f;    // cos(half angle), spot only
    uint8_t   type         = 0;       // HE::LightType
    uint8_t   envRole      = 0;       // 0 none, 1 = environment sun, 2 = environment moon
    bool      castsShadow  = false;   // authored LightComponent::castsShadow
    int16_t   shadowLayer  = -1;      // point/spot shadow map: first layer in the local
                                      // shadow atlas (spot: 1 layer, point: 6 cube-face
                                      // layers), assigned by the extractor; -1 = none
};

// Directional-light shadow info, computed by the extractor.
//   viewProj   — single whole-scene light clip transform. No backend samples a
//                single map any more (D3D12 and Vulkan were the last, Thema 35);
//                it still drives the render graph's ShadowPass caster cull.
//   cascade*   — Cascaded Shadow Maps: `cascadeCount` tight light frusta fit to
//                successive camera-distance slices (sharp near, coarse far), used by
//                every hardware backend (Metal, OpenGL, D3D11, D3D12, Vulkan).
//                cascadeSplit[i] = the cascade's far
//                distance in view space (camera-forward metres) for per-fragment
//                cascade pick.
struct ShadowData {
    glm::mat4 viewProj   = glm::mat4(1.0f);
    glm::vec3 direction  = glm::vec3(0.0f, -1.0f, 0.0f);
    bool      enabled    = false; // true when a directional light is present

    static constexpr int kMaxCascades = 4;
    int       cascadeCount = 0;
    glm::mat4 cascadeViewProj[kMaxCascades] = { glm::mat4(1.0f), glm::mat4(1.0f),
                                                glm::mat4(1.0f), glm::mat4(1.0f) };
    float     cascadeSplit[kMaxCascades]    = { 0.0f, 0.0f, 0.0f, 0.0f };

    // ── Local-light (point/spot) shadow maps ────────────────────────────────
    // Independent of the directional CSM above. The extractor picks the
    // camera-nearest shadow-casting point/spot lights (LightComponent::
    // castsShadow) and packs their depth views into ONE 2D depth-array
    // "local shadow atlas": a spot light uses 1 layer (perspective map along
    // its cone), a point light uses 6 layers (cube faces stored as array
    // layers, +X −X +Y −Y +Z −Z — face picked in the shader by major axis, so
    // the same code works on every backend without cube-depth samplers).
    // LightData::shadowLayer is the light's first layer; localViewProj[layer]
    // is that view's light clip transform (GL clip, z∈[-1,1] — backends apply
    // their own clip fix, exactly like cascadeViewProj).
    static constexpr int kMaxLocalShadowLayers = 16;
    int       localLayerCount = 0;
    glm::mat4 localViewProj[kMaxLocalShadowLayers] = {};
    // Per layer: the entity that OWNS the light this layer belongs to (see
    // LightData::entityId). The depth pass skips that entity's own geometry for
    // this layer only. kNoOwnerEntity = skip nothing (all CSM cascades).
    uint32_t  localOwnerEntity[kMaxLocalShadowLayers] = {};
};

// One live particle's raw GPU-instanced draw data — position/size (still CPU-lerped,
// a single scalar isn't worth moving to the GPU) plus t01 (0=born..1=dead), which the
// backend's baked/on-demand-compiled ParticleGraph shader turns into color+alpha via
// HE::generateParticleShaderSource's heParticleColor/heParticleAlpha (see
// HorizonRendering::ParticleShaderTemplates). Replaces the old one-RenderObject-per-
// particle path (RenderObject::instanceTint) for camera-billboard ParticleSystemComponent
// particles specifically — that field/path stays valid for everything else (weather
// precipitation billboards, foliage, future per-instance-varying-appearance needs).
struct ParticleInstance {
    glm::vec3 position;
    float     size;
    float     t01;
};

// One ParticleSystemComponent's live particles, batched for ONE GPU-instanced draw
// call. `config` carries the resolved color/alpha-over-life endpoints the backend
// hashes/bakes a shader from (see ContentManager::getParticleGraph's resolved
// config) — NOT re-evaluated from the graph, so it always matches what the CPU
// simulation (ParticleSystem::stepPool) already committed to.
struct ParticleBatch {
    HE::UUID                      particleAssetId; // the ParticleGraphAsset — backend looks up
                                                    // ContentManager::getParticleGraph(id)->
                                                    // precompiledShaders here before falling
                                                    // back to an on-demand-compiled + cached shader
    HE::UUID                      meshAssetId;     // usually the default quad
    HE::UUID                      materialAssetId; // base texture (heTex0), optional
    HE::ParticleEmitterConfig     config;
    uint32_t                      entityId = 0;
    std::vector<ParticleInstance> instances;
};

// One projected decal (DecalComponent), extracted per frame. The transform is
// the entity's world matrix applied to a unit cube [-0.5, 0.5]³; the deferred
// path blends color (× texture) into the G-buffer base colour inside the box.
struct DecalData {
    glm::mat4 transform = glm::mat4(1.0f);
    glm::vec4 color     = glm::vec4(1.0f);
    HE::UUID  textureId;
};

// One TrailComponent's band, already triangulated, in WORLD coordinates.
//
// Deliberately NOT an asset, and that is the whole point of the split between a
// rope and a trail (docs/rope-trail-plan.md §2 / §6.2): a trail's geometry
// changes every frame, and replacing a runtime StaticMeshAsset every frame would
// free and rebuild the mesh's BLAS *and* throw away the scene's entire software
// BVH each time (MetalRenderer.mm's InvalidateMesh drain — the SW-RT ranges live
// in concatenated arrays and a single mesh cannot be cut out of them). A rope
// pays that only when somebody moves it; a trail would pay it sixty times a
// second, for the whole scene.
//
// So the backends treat this like the debug lines: CPU vertices into a dynamic
// buffer, one draw, no UUID and no cache invalidation. The vertex layout is
// deliberately identical to the cooked mesh format (pos3 + norm3 + uv2), so the
// ordinary material path — the same vertex shader, the same PBR/graph pipelines,
// the same alpha-blended transparency pass — draws it with no shader of its own.
// The AGE rides in uv.v (0 at the tip, 1 at the tail); a material graph reads it
// there, which is why TrailComponent has no colour-over-life fields.
//
// World coordinates mean the model matrix is the identity. Trails never enter
// the shadow pass, the SSAO prepass or an acceleration structure — same reason
// as the precipitation billboards.
struct RibbonBatch {
    HE::UUID              materialAssetId;
    std::vector<float>    vertices;   // vertexCount * 8: pos3 + norm3 + uv2
    std::vector<uint32_t> indices;
    uint32_t              entityId = 0;
    HE::AABB              worldBounds;
};

// Move a ribbon's WORLD-space positions onto their own centre and return that
// centre — the translation the caller then draws them with as the model matrix.
//
// Only the DrawCall backends (Vulkan, D3D11, D3D12) need this, and the reason is
// worth spelling out because Metal and GL deliberately do it differently: those
// two replay ribbons through a draw list of their own, so they compute the
// blended pass's sort key themselves and can keep the identity model. The
// DrawCall backends hand ribbons to the SHARED blended pass, which sorts with
// RenderSorter::backToFrontKey — the model matrix's translation column. An
// identity model there puts every trail in the scene at the world origin, and
// they all sort the same.
//
// translate(c) * (p - c) == p, so the world position the shader reconstructs is
// bit-for-bit the intent, and a pure translation leaves normals — and therefore
// the shading — untouched. Positions are the first 3 of every 8 floats; normals
// and UVs are copied through.
inline glm::vec3 rebaseRibbonVertices(const RibbonBatch& batch, std::vector<float>& out)
{
    out = batch.vertices;
    if (!batch.worldBounds.isValid()) return glm::vec3(0.0f);
    const glm::vec3 c = batch.worldBounds.center();
    for (size_t i = 0; i + 8 <= out.size(); i += 8)
    {
        out[i + 0] -= c.x;
        out[i + 1] -= c.y;
        out[i + 2] -= c.z;
    }
    return c;
}

// The instances one cluster RenderObject stands for (RenderObject::instanceBlock
// indexes RenderWorld::instanceBlocks). Foliage lives here instead of as an object
// per plant: culling, sorting and material resolve see ONE object per bucket, and
// only GeometryPass / RenderSorter::batchDepthRuns walk the matrices when they form
// a draw.
struct InstanceBlock {
    // How `parent` combines with the stored matrices. Identity and Translation are the
    // overwhelmingly common terrain poses and unfold with copies and three adds; General
    // is a full matrix product. All three give the same bits a hand-built world matrix would.
    enum class Parent : uint8_t { Identity, Translation, General };

    // Owns the storage for as long as ANY copy of the RenderWorld lives: a frame copy
    // (RenderExtractor::beginFrame), a retained world, a world that outlives the
    // re-scatter it was extracted from keep reading exactly the matrices they got.
    std::shared_ptr<const std::vector<glm::mat4>> matrices;
    uint32_t  first  = 0;   // this block's instances: matrices[first, first + count)
    uint32_t  count  = 0;
    glm::mat4 parent = glm::mat4(1.0f);   // world = parent * matrices[i]; the terrain's world matrix
    Parent    kind   = Parent::Identity;

    // Classify a parent matrix (exact comparison: an almost-identity rotation is General).
    static Parent classify(const glm::mat4& m)
    {
        if (m[0] != glm::vec4(1, 0, 0, 0) || m[1] != glm::vec4(0, 1, 0, 0) ||
            m[2] != glm::vec4(0, 0, 1, 0) || m[3].w != 1.0f)
            return Parent::General;
        return (m[3].x == 0.0f && m[3].y == 0.0f && m[3].z == 0.0f) ? Parent::Identity
                                                                   : Parent::Translation;
    }

    // False for a block whose range does not fit its storage (a hand-built or corrupt
    // one): the readers then treat the object as the single plant `transform` says.
    bool valid() const
    {
        return matrices && count > 0 && static_cast<size_t>(first) + count <= matrices->size();
    }

    // Only the world POSITION of instance `i` (world(i)[3]), bit for bit what world(i) puts there
    // — for a test that rejects most instances before it pays for the matrix (the draw-distance
    // test of FoliageExtract). Identity and Translation read three floats; the general case is the
    // full product, because anything cheaper might round differently.
    glm::vec3 position(uint32_t i) const
    {
        const glm::mat4& m = (*matrices)[static_cast<size_t>(first) + i];
        switch (kind)
        {
        case Parent::Identity:    return glm::vec3(m[3]);
        case Parent::Translation: return glm::vec3(m[3]) + glm::vec3(parent[3]);
        default:                  return glm::vec3((parent * m)[3]);
        }
    }

    // The world matrix of instance `i` of this block, bit-identical on every path.
    glm::mat4 world(uint32_t i) const
    {
        const glm::mat4& m = (*matrices)[static_cast<size_t>(first) + i];
        switch (kind)
        {
        case Parent::Identity:    return m;
        case Parent::Translation: { glm::mat4 w = m; w[3] = glm::vec4(glm::vec3(m[3]) + glm::vec3(parent[3]), 1.0f); return w; }
        default:                  return parent * m;
        }
    }
};

// The world matrices of a block's `count` instances, world(0..count-1) bit for bit,
// computed ONCE per block and kept across frames: foliage does not move, and every
// pass of every frame used to multiply (or copy) each plant again, five times a frame
// (three cascades, SSAO, scene; docs/perf-audit/frame-parallelism-audit-2026-10-26.md
// lever 1). An Identity parent needs no copy: the pointer goes straight into the
// block's storage. The cache holds a reference to that storage, so a re-scatter
// that frees it can never alias a stale entry; instanceWorldCacheNextFrame() drops
// what no frame has asked for in a while. Thread-safe.
HE_RENDERING_API const glm::mat4* instanceWorldMatrices(const InstanceBlock& block);
// Call once per frame before any pass reads spans (the Metal frame does): entries
// not used for a few frames are released.
HE_RENDERING_API void instanceWorldCacheNextFrame();
// Entries and matrices held (tests, diagnostics).
HE_RENDERING_API size_t instanceWorldCacheEntries();

class RenderWorld {
public:
    void clear();

    // The dominant directional light: the BRIGHTEST directional light in the
    // extracted set — the same light the shadow fit and the fragment loop use.
    //
    // NEVER `sunDirection` (that is the SKY-DOME sun, which sits below the horizon
    // at night and never tracks a user-placed key light: rays traced toward it zero
    // the actual directional light almost everywhere — the scene goes black and only
    // surfaces facing the below-horizon sun light up, e.g. a bright cube UNDERSIDE
    // at night). And NEVER the raw environment sunColor, which is unmodulated by
    // night/clouds.
    //
    // In day-night scenes the sun/moon are themselves lights in `lights` (envRole
    // 1/2), so this pick follows them; the fallback below only fires in scenes with
    // no directional light at all.
    //
    // Returns false when nothing shines (night without a moon, full overcast zeroing
    // sun AND moon, or a light-less scene). Then `towardOut` is a harmless
    // placeholder (the shadow mask multiplies nothing) but `colorIntensityOut` is
    // hard ZERO: falling back to the environment's sunColor*sunIntensity here would
    // feed the probe bounce full DAYTIME sunlight from below the horizon — meshes
    // visibly sun-lit at night.
    //
    //   towardOut         — normalized direction TOWARD the light
    //                       (LightData::direction is the light's TRAVEL direction)
    //   colorIntensityOut — color * intensity
    // Exported per-member (not per-class): the backend static libraries include this
    // header but do not link HorizonRendering, so only the symbols they actually
    // call may carry the import/export attribute.
    HE_RENDERING_API bool dominantDirectionalLight(glm::vec3& towardOut,
                                                   glm::vec3& colorIntensityOut) const;

    // ── Instances a draw item stands for ────────────────────────────────────
    // The block of a cluster object, or nullptr for an ordinary one (and for a
    // cluster whose block is missing or does not fit its storage: such an object
    // reads as the single plant its `transform` names).
    const InstanceBlock* blockOf(const RenderObject& obj) const
    {
        if (!obj.isCluster() || static_cast<size_t>(obj.instanceBlock) >= instanceBlocks.size())
            return nullptr;
        const InstanceBlock& b = instanceBlocks[static_cast<size_t>(obj.instanceBlock)];
        return b.valid() ? &b : nullptr;
    }

    // 1 for an ordinary object, the block's count for a cluster.
    uint32_t instanceCountOf(const RenderObject& obj) const
    {
        const InstanceBlock* b = blockOf(obj);
        return b ? b->count : 1u;
    }

    // Instance i of obj as a world matrix: its own transform for an ordinary object (i is 0), the
    // block's i-th plant (parent applied) for a cluster. A pure function of (obj, i), so a draw can
    // read its matrices straight from here, on any thread, instead of unfolding into a list first.
    // All of obj's instances as a run (see InstanceRun): no copy, no product.
    InstanceRun instanceRun(const RenderObject& obj) const
    {
        const InstanceSpan s = instanceSpan(obj, 0, instanceCountOf(obj));
        return InstanceRun(nullptr, 0, s.src, s.n);
    }

    // Append the WORLD matrices of obj's instances to `out`: its own transform for an
    // ordinary object, the whole block (parent applied) for a cluster. The one place
    // that unfolds a cluster; GeometryPass and RenderSorter::batchDepthRuns call it.
    void appendInstances(const RenderObject& obj, std::vector<glm::mat4>& out) const
    {
        appendInstanceRange(obj, 0, instanceCountOf(obj), out);
    }

    // Instances [from, from + n) of obj, for a caller that cuts one cluster across two
    // draws (an ordinary object is instance 0 of one).
    void appendInstanceRange(const RenderObject& obj, uint32_t from, uint32_t n,
                             std::vector<glm::mat4>& out) const
    {
        if (n == 0) return;
        const size_t base = out.size();
        out.resize(base + (blockOf(obj) ? n : 1u));
        std::vector<InstancePiece> pieces;
        addInstancePiece(pieces, obj, from, n, 0);
        writeInstancePieces(pieces, out.data() + base);
    }

    // Unfolding on the job system's workers. A caller lays out where each object's instances go
    // (addInstancePiece, serial and cheap), sizes its array once, and writeInstancePieces fills it.
    // At 300k plants the five unfoldings a frame (three cascades, SSAO, scene) ran on the main
    // thread with ten workers idle: the matrix products, and copies through every doubling of
    // the lists (docs/perf-audit/frame-parallelism-audit-2026-10-26.md, lever 1). A big block is
    // cut into pieces so that one cluster spreads too, many small ones share out as they are.
    // Every instance is still world(i), so the bits are the same on either path.
    struct InstancePiece
    {
        const RenderObject* obj;
        uint32_t            from;   // first instance of obj
        uint32_t            n;
        size_t              pos;    // where in the destination array
    };
    static constexpr uint32_t kInstancePieceMax  = 2048;
    static constexpr size_t   kParallelUnfoldMin = 4096;   // fewer instances stay on the caller
    // HE_PARALLEL_UNFOLD=0 keeps every unfolding on the calling thread: the A/B switch, like
    // HE_DEPTH_INSTANCING (same bits either way).
    static bool parallelUnfoldEnabled()
    {
        static const bool on = [] {
            const char* v = std::getenv("HE_PARALLEL_UNFOLD");
            return !(v && *v && std::atoi(v) == 0);
        }();
        return on;
    }
    void addInstancePiece(std::vector<InstancePiece>& pieces, const RenderObject& obj,
                          uint32_t from, uint32_t n, size_t pos) const
    {
        if (!blockOf(obj)) { if (n > 0) pieces.push_back({ &obj, 0, 1, pos }); return; }
        while (n > kInstancePieceMax)
        {
            pieces.push_back({ &obj, from, kInstancePieceMax, pos });
            from += kInstancePieceMax; pos += kInstancePieceMax; n -= kInstancePieceMax;
        }
        if (n > 0) pieces.push_back({ &obj, from, n, pos });
    }
    void writeInstancePieces(const std::vector<InstancePiece>& pieces, glm::mat4* dst) const
    {
        auto one = [this, &pieces, dst](size_t p)
        {
            const InstancePiece& x = pieces[p];
            const InstanceBlock* b = blockOf(*x.obj);
            if (!b) { dst[x.pos] = x.obj->transform; return; }
            for (uint32_t i = 0; i < x.n; ++i) dst[x.pos + i] = b->world(x.from + i);
        };
        // Only cluster instances count toward going wide: an ordinary object is one matrix
        // copy, and 16k of them (the 50k reference world) were faster on the caller than shared out.
        size_t clustered = 0;
        for (const InstancePiece& x : pieces) if (x.obj->isCluster()) clustered += x.n;
        if (pieces.size() > 1 && clustered >= kParallelUnfoldMin && parallelUnfoldEnabled())
            parallel_for(pieces.size(), one, "UnfoldInstances", 1);
        else
            for (size_t p = 0; p < pieces.size(); ++p) one(p);
    }

    // Instances [from, from + n) of obj as one span (see InstanceSpan): no copy, no product.
    InstanceSpan instanceSpan(const RenderObject& obj, uint32_t from, uint32_t n) const
    {
        const InstanceBlock* b = blockOf(obj);
        if (!b) return { &obj.transform, 1u };
        return { instanceWorldMatrices(*b) + from, n };
    }

    // The same walk without materialising a list: call fn(world matrix) per instance.
    template <class Fn>
    void forEachInstance(const RenderObject& obj, Fn&& fn) const
    {
        const InstanceBlock* b = blockOf(obj);
        if (!b) { fn(obj.transform); return; }
        for (uint32_t i = 0; i < b->count; ++i) fn(b->world(i));
    }

    std::vector<RenderObject>        objects;
    std::vector<SkinnedRenderObject> skinnedObjects;
    // Storage behind RenderObject::instanceBlock (foliage clusters); empty without foliage.
    std::vector<InstanceBlock>       instanceBlocks;
    std::vector<LightData>           lights;
    std::vector<DecalData>           decals;
    std::vector<UIRenderObject>      uiObjects;
    std::vector<ParticleBatch>       particleBatches;
    // Motion trails, retriangulated every frame — see RibbonBatch above. Drawn
    // by the backends in the transparency pass, after the opaque scene and the
    // sky, with the identity model matrix.
    std::vector<RibbonBatch>         ribbonBatches;
    // Painted landscapes, referenced by RenderObject::landscapeIndex. Only the GI
    // reflection kernels read these (to sample the paint per ray hit) — see
    // GiLandscape.h. Empty in scenes without a terrain.
    std::vector<HE::GiLandscape>     landscapes;
    CameraData                camera;
    ShadowData                shadow;
    UnderwaterCameraData      underwaterCamera;

    // Direction *toward* the sun (normalized), set by the extractor: from the
    // first directional light, or driven by the day-night cycle when enabled.
    // Backends use it for the procedural sky + image-based ambient.
    glm::vec3 sunDirection = glm::vec3(0.45f, 0.80f, 0.55f);

    // Flat ambient fill added to every lit surface, set by the extractor. A weak
    // floor is always present (so the scene is never fully black); under heavy
    // cloud cover it grows to replace the switched-off sun/moon directional light.
    glm::vec3 ambient = glm::vec3(0.03f, 0.035f, 0.05f);
};
