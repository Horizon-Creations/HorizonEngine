#pragma once
#include <cstdint>
#include <Renderer/IRenderer.h>

struct D3D12RendererImpl;

class D3D12Renderer : public IRenderer
{
public:
    D3D12Renderer();
    ~D3D12Renderer();
    void Initialize(HE::Window* window) override;
    void Shutdown()                      override;
    void Render()                        override;
    Capabilities GetCapabilities() const override;

    // Native handle accessors — void* so D3D12 headers stay out of this header.
    // Cast to ID3D12Device* / ID3D12CommandQueue* at the call site.
    void* GetDevice()       const;
    void* GetCommandQueue() const;
    void  SetVSync(bool enabled) override;

    // Offscreen viewport (editor scene view)
    void  SetViewportSize(uint32_t width, uint32_t height) override;
    // GetViewportTexture() is inherited from IRenderer and returns m_viewportImGuiHandle.
    bool  CaptureViewport(std::vector<uint8_t>& rgba,
                          uint32_t& width, uint32_t& height) override;
    // Returns ID3D12Resource* for the viewport color RT (or nullptr if not allocated).
    // The editor allocates an SRV in its ImGui heap and calls SetViewportImGuiHandle.
    void* GetViewportD3DResource() const;
    // True when SetViewportSize changed the RT size since the last call to
    // ClearViewportResourceChanged(). The editor checks this to re-register the SRV.
    bool  HasViewportResourceChanged() const;
    void  ClearViewportResourceChanged();

    void SetDebugLines(const std::vector<DebugLine>& lines) override;
    void SetMoonTexture(const void* rgba8Pixels, int width, int height) override;
    void SetSSAOSettings(const SSAOSettings& settings) override;
    void SetBloomSettings(const BloomSettings& settings) override;
    void SetAntiAliasingSettings(const AntiAliasingSettings& settings) override;
    // Ray-traced DDGI (software BVH + CS 5.0 compute) — pushed every frame by
    // the editor prefs / packaged game, mirroring the Metal/GL/D3D11 backends.
    void SetGISettings(const GISettings& settings) override;

    // Editor material/mesh hot-reload: drop the cached override-material texture / mesh
    // GPU state so the next frame re-resolves it from the ContentManager (mirrors GL/Metal).
    void InvalidateMaterial(const HE::UUID& materialId) override;
    void InvalidateMesh(const HE::UUID& meshId) override;

    // Whole-frame GPU time from a per-frame-in-flight timestamp query pair
    // (read back k_frameCount frames late so it never stalls) + CPU counters,
    // plus a per-pass breakdown from extra timestamp pairs in the same heap
    // slot (Shadow / SSAO / Opaque / Sky+Clouds / Transparent / Bloom /
    // Tonemap / FXAA|SMAA|AA Resolve / UI). The whole-frame pair is recorded
    // unconditionally; the per-pass stamps only while the profiler is
    // RECORDING, latched once per frame in Render(). gpuTimingMode reports
    // "d3d12-timer" only when per-pass rows are actually in hand — the rows
    // arrive k_frameCount frames late, so it reads "whole-frame" for the first
    // few frames of a capture and again once the capture stops.
    FrameGpuStats GetFrameGpuStats() const override;

    // Build the node-graph material PSOs ahead of their first draw. Only the
    // (non-HDR, opaque) variant — neither `usingHDR` nor RenderSorter::isTransparent
    // can be keyed correctly before the first frame; see the definition's comment.
    void WarmupMaterials(const std::vector<HE::UUID>& materialIds) override;

    // Content-Browser tiles, rendered into a PRIVATE target and read back as
    // tightly packed TOP-DOWN RGBA8. Callable before the first Render(): they own
    // their target, their descriptor heaps and their own allocator/command-list
    // pair, and depend on no Render()-created resource. All cached in
    // D3D12RendererImpl and torn down in Shutdown().
    bool RenderAssetThumbnail(ContentManager& cm, ThumbnailKind kind, const HE::UUID& assetId,
                              uint32_t size, std::vector<uint8_t>& outRgba8) override;
    bool RenderWidgetThumbnail(const std::vector<UIRenderObject>& uiObjects, uint32_t size,
                               std::vector<uint8_t>& outRgba8) override;
    bool RenderParticleThumbnail(ContentManager& cm, const HE::UUID& materialId,
                                 const std::vector<ParticlePreviewInstance>& particles,
                                 uint32_t size, std::vector<uint8_t>& outRgba8) override;

    // ── Interactive asset previews (P1c) ──────────────────────────────────────
    // Unlike the tiles above these return an ImGui texture handle rather than
    // pixels: the editor panel samples the target directly, every frame, for as
    // long as its tab is open. Each preview owns a SEPARATE target — sharing one
    // would mean a Content-Browser tile, or the second preview the Class Editor
    // draws in the same frame, replacing what the first one is showing.
    //
    // The returned handle is registered with the editor's ImGui SRV heap ONCE per
    // target lifetime and cached, because that heap is a fixed 64 descriptors and
    // DestroyImGuiTexture never gives one back. The requested size is rounded up to
    // a multiple of 64 before it is compared against the live target, so dragging a
    // panel splitter does not recreate the target (and leak a descriptor) on every
    // pixel of movement. Returns nullptr when the asset cannot be resolved or the
    // ImGui heap is exhausted; the panel then shows its placeholder.
    //
    // Callable before the first Render(), like the tile paths: own target, own
    // heaps, own allocator/command-list pair.
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

    // ImGui editor textures (content-browser icons + logo). Uploads the RGBA8
    // pixels to a GPU texture, then hands the resource to the editor-installed
    // registrar (m_imguiTexRegistrar) which builds the ImGui SRV. Returns the
    // ImTextureID-compatible handle, or nullptr if no registrar is installed.
    void* CreateImGuiTexture(const void* rgba8Pixels, int width, int height) override;
    void  DestroyImGuiTexture(void* handle) override;

    // ── Multi-window (P1d) ────────────────────────────────────────────────────
    // A real per-window DXGI swapchain + its own RTV heap and its own
    // allocator/command-list pair; RenderWindow CLEARS and PRESENTS it. It does
    // NOT draw scene content — no backend does (GL carries a literal
    // "TODO: secondary-window draw calls", Vulkan/Metal only clear), so this is
    // parity, not a shortfall. AttachWindow never throws: it is reached from
    // Application::createSecondaryWindow, which is called from OUTSIDE Run()'s
    // try/catch, so a failure logs and leaves the window unattached instead of
    // tearing down the app. See the definitions for the full rationale.
    void AttachWindow(HE::Window* window) override;
    void DetachWindow(HE::Window* window) override;
    void RenderWindow(HE::Window* window) override;

private:
    // Extract → cull → sort → RenderGraph → replay into the bound command list.
    void DrawScene(void* cmdList, int width, int height);

    D3D12RendererImpl* m_impl = nullptr;
};
