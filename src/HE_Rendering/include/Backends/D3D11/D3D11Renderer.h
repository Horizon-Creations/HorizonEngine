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
    // One still from another camera (MCP scene_screenshot), the Metal/GL
    // contract: the live viewport pair is set aside, one viewport frame is drawn
    // into a fresh pair at the requested size and read back, nothing is presented.
    bool  RenderSceneImage(const EditorCameraOverride& camera, uint32_t width, uint32_t height,
                           std::vector<uint8_t>& rgba) override;
    // Runtime Virtual Texture page pool (Thema 333); RenderSceneImage fills a slice when the camera says so.
    bool  CreateRvtPool(uint32_t slots, uint32_t pagePx) override;
    void  DestroyRvtPool() override;
    bool  ReadbackRvtSlot(int layer, uint32_t slot, std::vector<uint8_t>& rgba) override;
    bool  SetRvtPageTable(const HE::UUID& consumerMaterial, const std::vector<std::vector<uint32_t>>& mips) override;
    bool  ReadbackRvtPageTable(const HE::UUID& consumerMaterial, std::vector<std::vector<uint32_t>>& mips) override;
    void  SetRvtLandscape(uint32_t terrainEntity, const HE::UUID& landscapeMaterial, const HE::UUID& consumerMaterial) override;
    void  ReleaseRvtConsumer(const HE::UUID& consumerMaterial) override;
    // One page through the lean page pass (RenderSceneImage routes camera.rvtSlot >= 0 here).
    bool  RenderRvtPage(const EditorCameraOverride& camera, uint32_t px);
    // Once per real frame (Render()): ages pages, harvests finished BC7 encodes/readbacks of pages gone cold, starts new
    // readbacks for pages that have stood still, re-uploads page tables whose entries changed temperature (Thema 333 step 9).
    void  UpdateRvtCold();
    // An arbitrary world into a per-slot offscreen target (Class Editor, Mesh
    // viewer, secondary Scene viewports) — the GL/Metal contract in IRenderer.h.
    // Returns the target's ID3D11ShaderResourceView*.
    void* RenderWorldPreview(ContentManager& cm, HorizonWorld& world,
                             uint32_t width, uint32_t height,
                             const EditorCameraOverride& camera,
                             const glm::vec3& origin = glm::vec3(0.0f),
                             const WorldPreviewEnv& env = {},
                             glm::mat4* outViewProj = nullptr,
                             uint32_t slot = 0) override;
    // The packaged game: Render() runs the viewport frame at the back buffer's
    // size and copies its result into the back buffer (docs/spielpfad-postfx-
    // parity-analyse-2026-10-03.md, Weg a).
    void  SetSwapchainPostProcessing(bool enabled) override;

    // [blind] added D3D11 sky+IBL+debuglines parity
    void SetDebugLines(const std::vector<DebugLine>& lines) override;
    void SetMoonTexture(const void* rgba8Pixels, int width, int height) override;
    void SetSSAOSettings(const SSAOSettings& settings) override;
    void SetBloomSettings(const BloomSettings& settings) override;
    void SetAntiAliasingSettings(const AntiAliasingSettings& settings) override;
    // Software ray-traced DDGI (CPU BVH + CS 5.0) — mirrors the GL 4.3/Vulkan port.
    void SetGISettings(const GISettings& settings) override;
    // Forward screen-space reflections (docs/ssr-cross-backend-plan.md checkpoint C).
    // Post-chain only: the trace reads the previous frame's HDR colour, which the
    // editor viewport and the packaged game (SetSwapchainPostProcessing) have and
    // the direct swapchain fallback does not.
    void SetSSRSettings(const SSRSettings& settings) override;
    // GI reflections (Thema 191 step 2): ray-traced specular off the same
    // software BVH diffuse GI uses, mirrors D3D12/GL. Reaches the image
    // through graph materials only (heGIReflFwd, t32) — the built-in PBR
    // shader has no stage for it yet.
    void SetGIReflectionSettings(const GIReflectionSettings& settings) override;
    // Cascaded shadow maps (project ShadowSettings) + the per-cascade debug
    // tint — the same contract GL and Metal honour.
    void SetShadowSettings(const ShadowSettings& settings) override;
    void SetShadowDebug(bool on) override;

    // Editor material/mesh hot-reload: drop the cached override-material texture / mesh
    // GPU state so the next frame re-resolves it from the ContentManager (mirrors GL/Metal).
    void InvalidateMaterial(const HE::UUID& materialId) override;
    void InvalidateMesh(const HE::UUID& meshId) override;
    // Build node-graph material shaders ahead of their first draw (queued, drained at
    // the top of the next DrawScene so the build lands on the render thread with the
    // material resources up) — mirrors GL/Metal; cache hits are free.
    void WarmupMaterials(const std::vector<HE::UUID>& materialIds) override;
    bool SupportsTessellation() const override;
    // Editor texture hot-reload: drop a graph project texture (heTexP slot) so the
    // next material draw re-uploads it.
    void InvalidateTexture(const HE::UUID& textureId) override;

    // Whole-frame D3D11 timestamp timing (double-buffered ring, never stalls)
    // + this frame's CPU draw/triangle/visibility counters.
    FrameGpuStats GetFrameGpuStats() const override;

private:
    // Extract → cull → sort → RenderGraph → replay into the currently bound targets.
    void DrawScene(int width, int height);
    // The offscreen viewport frame: scene into HDR (or straight into the viewport
    // target), bloom, tonemap, AA resolve, UI canvas — everything up to the
    // viewport texture ImGui samples, nothing of the swapchain. Shared by Render()
    // and RenderSceneImage(); the caller has already made sure the viewport pair
    // exists.
    void DrawViewportFrame();

    D3D11RendererImpl* m_impl = nullptr;
};
