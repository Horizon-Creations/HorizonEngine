#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <Renderer/IRenderer.h>

// Passed as the overlay-callback context so ImGui (or any other overlay) can
// encode into the active render pass. All pointers are Objective-C objects
// (__bridge-casted) and only valid for the duration of the callback.
struct MetalOverlayContext
{
	void* commandBuffer;         // id<MTLCommandBuffer>
	void* renderEncoder;         // id<MTLRenderCommandEncoder>
	void* renderPassDescriptor;  // MTLRenderPassDescriptor*
};

class MetalRendererImpl;

// The Metal backend behind a Pimpl, like D3D11Renderer/D3D12Renderer (audit
// REN-301): every member and helper is MetalRendererImpl's, declared in
// src/Backends/Metal/MetalRendererImpl.h and implemented in MetalRenderer.mm
// (Objective-C++). This header stays plain C++ so RendererFactory and the editor
// can include it from .cpp files, and a change to the backend's state no longer
// recompiles them.
class MetalRenderer : public IRenderer
{
public:
	MetalRenderer();
	~MetalRenderer() override;

	void Initialize(HE::Window* window) override;
	void Shutdown()                      override;
	void Render()                        override;
	void WaitForFrame()                  override;
	Capabilities GetCapabilities() const override;
	FrameGpuStats GetFrameGpuStats() const override;

	void SetVSync(bool enabled) override;

	void* CreateImGuiTexture(const void* rgba8Pixels, int width, int height) override;
	void  DestroyImGuiTexture(void* handle) override;
	void  SetMoonTexture(const void* rgba8Pixels, int width, int height) override;

	void  SetViewportSize(uint32_t width, uint32_t height) override;
	void* GetViewportTexture() override;
	bool  CaptureViewport(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) override;
	bool  RenderSceneImage(const EditorCameraOverride& camera, uint32_t width, uint32_t height,
	                       std::vector<uint8_t>& rgba) override;
	void  InvalidateMaterial(const HE::UUID& materialId) override;
	void  WarmupMaterials(const std::vector<HE::UUID>& materialIds) override;
	void* RenderMaterialPreview(ContentManager& cm, const HE::UUID& materialId,
	                            uint32_t size, float yaw, float pitch, float dist,
	                            int shape = 0, const HE::UUID& meshId = HE::UUID{},
	                            float timeSeconds = -1.0f) override;
	void* RenderSkeletalPreview(ContentManager& cm, const HE::UUID& meshId,
	                            const std::vector<glm::mat4>& boneMatrices,
	                            uint32_t width, uint32_t height,
	                            float yaw, float pitch, float dist,
	                            bool showSkeleton = true,
	                            glm::mat4* outViewProj = nullptr) override;
	void* RenderWorldPreview(ContentManager& cm, HorizonWorld& world,
	                         uint32_t width, uint32_t height,
	                         const EditorCameraOverride& camera,
	                         const glm::vec3& origin = glm::vec3(0.0f),
	                         const WorldPreviewEnv& env = {},
	                         glm::mat4* outViewProj = nullptr,
	                         uint32_t slot = 0) override;
	void* RenderParticlePreview(ContentManager& cm, const HE::UUID& meshId, const HE::UUID& materialId,
	                            const std::vector<ParticlePreviewInstance>& particles,
	                            uint32_t size, float yaw, float pitch, float dist) override;
	bool  RenderAssetThumbnail(ContentManager& cm, ThumbnailKind kind, const HE::UUID& assetId,
	                           uint32_t size, std::vector<uint8_t>& outRgba8) override;
	bool  RenderParticleThumbnail(ContentManager& cm, const HE::UUID& materialId,
	                              const std::vector<ParticlePreviewInstance>& particles,
	                              uint32_t size, std::vector<uint8_t>& outRgba8) override;
	bool  RenderWidgetThumbnail(const std::vector<UIRenderObject>& uiObjects,
	                            uint32_t size, std::vector<uint8_t>& outRgba8) override;
	void  InvalidateMesh    (const HE::UUID& meshId)     override;
	void  InvalidateTexture (const HE::UUID& textureId)  override;
	void  SetBloomSettings(const BloomSettings& settings) override;
	void  SetDepthOfFieldSettings(const DepthOfFieldSettings& settings) override;
	void  SetMotionBlurSettings(const MotionBlurSettings& settings) override;
	void  SetSSAOSettings(const SSAOSettings& settings) override;
	void  SetShadowSettings(const ShadowSettings& settings) override;
	void  SetAntiAliasingSettings(const AntiAliasingSettings& settings) override;
	void  SetGISettings(const GISettings& settings) override;
	void  SetSSRSettings(const SSRSettings& settings) override;
	void  SetOcclusionCullingSettings(const OcclusionCullingSettings& settings) override;
	void  SetGIReflectionSettings(const GIReflectionSettings& settings) override;
	void  SetShadowDebug(bool on) override;
	void  SetGpuParticleParams(const GpuParticleParams& p) override;
	void  SetDebugLines(const std::vector<DebugLine>& lines) override;

	// Multi-window support
	void AttachWindow(HE::Window* window) override;
	void DetachWindow(HE::Window* window) override;
	void RenderWindow(HE::Window* window) override;

	// ── Accessors for the editor's ImGui Metal backend ─────────────────────
	void* GetDevice() const;              // id<MTLDevice>
	void* GetCommandQueue() const;        // id<MTLCommandQueue>
	// Render pass descriptor matching the swapchain format. Needed by
	// ImGui_ImplMetal_NewFrame() before the frame's real pass exists.
	void* GetFramePassDescriptor() const; // MTLRenderPassDescriptor*

private:
	// Reads and writes IRenderer's protected state (content manager, world, editor
	// camera, view mode) through its back-reference to this object.
	friend class MetalRendererImpl;
	std::unique_ptr<MetalRendererImpl> m_impl;
};
