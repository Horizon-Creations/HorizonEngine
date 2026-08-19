#pragma once
#include <cstdint>
#include <Renderer/IRenderer.h>

struct D3D11RendererImpl;

class D3D11Renderer : public IRenderer
{
public:
    D3D11Renderer();
    ~D3D11Renderer();
    void Initialize(HE::Window* window) override;
    void Shutdown()                      override;
    void Render()                        override;
    Capabilities GetCapabilities() const override;

    void* CreateImGuiTexture(const void* rgba8Pixels, int width, int height) override;
    void  DestroyImGuiTexture(void* handle) override;

    // Native handle accessors
    // Cast to ID3D11Device* / ID3D11DeviceContext* at the call site.
    void* GetDevice()  const;
    void* GetContext() const;
    void  SetVSync(bool enabled) override;

    // Offscreen viewport (editor scene view)
    void  SetViewportSize(uint32_t width, uint32_t height) override;
    void* GetViewportTexture() override; // returns ID3D11ShaderResourceView*
    bool  CaptureViewport(std::vector<uint8_t>& rgba,
                          uint32_t& width, uint32_t& height) override;

    // [blind] added D3D11 sky+IBL+debuglines parity
    void SetDebugLines(const std::vector<DebugLine>& lines) override;
    void SetMoonTexture(const void* rgba8Pixels, int width, int height) override;
    void SetSSAOSettings(const SSAOSettings& settings) override;
    void SetBloomSettings(const BloomSettings& settings) override;
    void SetAntiAliasingSettings(const AntiAliasingSettings& settings) override;
    // Software ray-traced DDGI (CPU BVH + CS 5.0) — mirrors the GL 4.3/Vulkan port.
    void SetGISettings(const GISettings& settings) override;

    // Editor material/mesh hot-reload: drop the cached override-material texture / mesh
    // GPU state so the next frame re-resolves it from the ContentManager (mirrors GL/Metal).
    void InvalidateMaterial(const HE::UUID& materialId) override;
    void InvalidateMesh(const HE::UUID& meshId) override;

    // Whole-frame D3D11 timestamp timing (double-buffered ring, never stalls)
    // + this frame's CPU draw/triangle/visibility counters.
    FrameGpuStats GetFrameGpuStats() const override;

    // Build the node-graph material VS/PS ahead of their first draw (one variant
    // per material — D3D11 has no PSO, so blend/depth are not baked in).
    void WarmupMaterials(const std::vector<HE::UUID>& materialIds) override;

    // Content-Browser tiles, rendered into a PRIVATE target and read back as
    // tightly packed TOP-DOWN RGBA8. Callable before the first Render(): they
    // create their own target and depend on no Render()-created resource.
    // The target and its pipelines are cached in D3D11RendererImpl.
    bool RenderAssetThumbnail(ContentManager& cm, ThumbnailKind kind, const HE::UUID& assetId,
                              uint32_t size, std::vector<uint8_t>& outRgba8) override;
    bool RenderWidgetThumbnail(const std::vector<UIRenderObject>& uiObjects, uint32_t size,
                               std::vector<uint8_t>& outRgba8) override;
    bool RenderParticleThumbnail(ContentManager& cm, const HE::UUID& materialId,
                                 const std::vector<ParticlePreviewInstance>& particles,
                                 uint32_t size, std::vector<uint8_t>& outRgba8) override;

    // Interactive asset previews. Each owns a SEPARATE persistent target (sharing
    // one would let a thumbnail — or a second preview — replace whatever an open
    // editor panel is currently showing) and returns that target's
    // ID3D11ShaderResourceView* as the ImGui handle. The SRV is created with the
    // target and cached, never per call. Callable before the first Render(), like
    // the thumbnails above.
    void* RenderMaterialPreview(ContentManager& cm, const HE::UUID& materialId, uint32_t size,
                                float yaw, float pitch, float dist,
                                int shape, const HE::UUID& meshId) override;
    void* RenderSkeletalPreview(ContentManager& cm, const HE::UUID& meshId,
                                const std::vector<glm::mat4>& boneMatrices,
                                uint32_t width, uint32_t height,
                                float yaw, float pitch, float dist,
                                bool showSkeleton, glm::mat4* outViewProj) override;
    void* RenderParticlePreview(ContentManager& cm, const HE::UUID& meshId,
                                const HE::UUID& materialId,
                                const std::vector<ParticlePreviewInstance>& particles,
                                uint32_t size, float yaw, float pitch, float dist) override;

private:
    // Extract → cull → sort → RenderGraph → replay into the currently bound targets.
    void DrawScene(int width, int height);

    D3D11RendererImpl* m_impl = nullptr;
};
