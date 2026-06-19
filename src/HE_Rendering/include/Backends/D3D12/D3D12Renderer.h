#pragma once
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

private:
    // Extract → cull → sort → RenderGraph → replay into the bound command list.
    void DrawScene(void* cmdList, int width, int height);

    D3D12RendererImpl* m_impl = nullptr;
};
