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
    // World-space bounds for culling. Invalid box = never culled, and that is
    // deliberately what the extractor leaves here for a mesh whose real AABB it
    // could not read yet (not resident): culling a large mesh against a small
    // proxy box makes it vanish while plainly in view. Backends fill in the real
    // bounds once the asset resolves. Proxy unit cubes are used only where the
    // size is known to be about right anyway (particles, skinned meshes).
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
    // Per-entity node-graph param override: empty = use the material's own
    // shaderParamData; otherwise the FULL merged HeParams block (16 vec4 = 64
    // floats) the backend uploads instead. Filled by the extractor from
    // MaterialComponent::paramOverrides; only set when the entity has overrides.
    std::vector<float> paramOverride;
    // Landscape layer weightmap for THIS object (terrain chunks only): the
    // painted per-texel layer weights of the chunk's parent landscape, bound so
    // a Landscape Layer Blend node can sample them. Null = not a landscape
    // chunk; the backend then binds the 1x1 layer-0 default so the node still
    // resolves. Per-OBJECT, not per-material: two landscapes can share one
    // material and still paint independently.
    HE::UUID     weightmapTextureId;
    // Mean of that weightmap over the whole landscape (TerrainComponent::
    // avgLayerWeights). The rasterizer samples the real texel and never needs
    // this; a consumer that shades per INSTANCE with no texel at all (the DDGI
    // probe bounce) blends the material's per-layer colours by this mean instead
    // of reflecting one arbitrary layer. { 1, 0, 0, 0 } (= layer 0, the shader's
    // unpainted default) for everything that is not a landscape chunk.
    glm::vec4    landscapeLayerWeights = { 1.0f, 0.0f, 0.0f, 0.0f };
    // Index into RenderWorld::landscapes, or -1. Set on terrain chunks only: it
    // is what lets the GI REFLECTION kernels reconstruct the hit's UV and sample
    // the paint per texel, instead of settling for the mean above (a mirrored
    // hillside would otherwise be one flat colour). See GiLandscape.h.
    int32_t      landscapeIndex = -1;
};

// Skinned renderable: same as RenderObject but carries bone matrices for GPU skinning.
struct SkinnedRenderObject : RenderObject {
    std::vector<glm::mat4> boneMatrices;
};
