#pragma once
#include <Types/Handle.h>
#include <Types/UUID.h>
#include <Math/Math.h>
#include <Math/AABB.h>
#include <cstdint>
#include <vector>

// One renderable entity extracted from the ECS world each frame.
struct RenderObject {
    // Asset identity straight from MeshComponent. Until the
    // RenderResourceManager resolves UUIDs to RenderHandles at extract time,
    // backends use this to look up / lazily upload the GPU mesh.
    HE::UUID     meshAssetId;
    // Optional material override straight from MaterialComponent. When set, the
    // backend resolves it instead of the mesh's embedded material. Null UUID =
    // fall back to the material baked into the mesh asset.
    HE::UUID     materialAssetId;
    RenderHandle meshHandle     = RenderHandle::invalid();
    RenderHandle materialHandle = RenderHandle::invalid();
    glm::mat4    transform      = glm::mat4(1.0f);
    // World-space bounds for culling. Seeded by the extractor with the
    // fallback cube's box; backends refine it with the real mesh AABB once
    // the asset is resolved. Invalid box = never culled.
    HE::AABB     worldBounds;
    uint32_t     entityId       = 0;
    uint8_t      lod            = 0;
    // PBR material scalars (resolved at extract time from MaterialAsset).
    glm::vec3    baseColor      = { 1.0f, 1.0f, 1.0f };
    float        metallic       = 0.0f;
    float        roughness      = 0.5f;
    float        opacity        = 1.0f;
    // Per-instance tint multiplied into the resolved material color/opacity at
    // draw time (rgb multiplies baseColor, a multiplies opacity) — identity
    // (1,1,1,1) is a complete no-op, so this only affects objects that actually
    // set it. Used for particle color/alpha-over-life (RenderExtractor resolves
    // it from ParticleEmitterConfig's Start/End Color/Alpha + each particle's
    // lifetime fraction); free for other per-instance-varying-appearance needs
    // later. GeometryPass only batches same-mesh+same-material runs that ALSO
    // share this value, so a shared DrawCall::instanceTint is always correct.
    glm::vec4    instanceTint   = { 1.0f, 1.0f, 1.0f, 1.0f };
    // Cheap-billboard opt-outs. Precipitation/particles set these false so thousands of
    // them skip the per-object shadow-map depth pass and the SSAO position prepass (where
    // they are NOT instanced) — and so rain/snow don't wrongly cast shadows or darken AO.
    bool         castsShadow    = true;
    bool         contributesAO  = true;
    // Terrain (entity carries a TerrainComponent). Terrain is a ground plane the
    // size of the whole level, so it must not drive the directional shadow map's
    // XY extent — one map stretched over the terrain collapses to a few texels per
    // prop. It still contributes to the light-space DEPTH range so its slope stays
    // inside the bias envelope. See the shadow fit in RenderExtractor.
    bool         isTerrain      = false;
    // Per-entity node-graph param override: empty = use the material's own
    // shaderParamData; otherwise the FULL merged HeParams block (16 vec4 = 64
    // floats) the backend uploads instead. Filled by the extractor from
    // MaterialComponent::paramOverrides; only set when the entity has overrides.
    std::vector<float> paramOverride;
};

// Skinned renderable: same as RenderObject but carries bone matrices for GPU skinning.
struct SkinnedRenderObject : RenderObject {
    std::vector<glm::mat4> boneMatrices;
};
