#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderObject.h"
#include "GiLandscape.h"
#include <Renderer/UIRenderObject.h>
#include <ParticleGraph/ParticleGraph.h>
#include <Math/Math.h>
#include <vector>
#include <cstdint>

struct CameraData {
    glm::mat4 view       = glm::mat4(1.0f);
    glm::mat4 projection = glm::mat4(1.0f);
    glm::vec3 position   = glm::vec3(0.0f);
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
//   viewProj   — single whole-scene light clip transform. Used by the backends that
//                are still on a single shadow map (D3D11 / D3D12 / Vulkan).
//   cascade*   — Cascaded Shadow Maps: `cascadeCount` tight light frusta fit to
//                successive camera-distance slices (sharp near, coarse far), used by
//                the Metal and OpenGL backends. cascadeSplit[i] = the cascade's far
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

    std::vector<RenderObject>        objects;
    std::vector<SkinnedRenderObject> skinnedObjects;
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

    // Direction *toward* the sun (normalized), set by the extractor: from the
    // first directional light, or driven by the day-night cycle when enabled.
    // Backends use it for the procedural sky + image-based ambient.
    glm::vec3 sunDirection = glm::vec3(0.45f, 0.80f, 0.55f);

    // Flat ambient fill added to every lit surface, set by the extractor. A weak
    // floor is always present (so the scene is never fully black); under heavy
    // cloud cover it grows to replace the switched-off sun/moon directional light.
    glm::vec3 ambient = glm::vec3(0.03f, 0.035f, 0.05f);
};
