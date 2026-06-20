#pragma once
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

private:
    // Extract → cull → sort → RenderGraph → replay into the currently bound targets.
    void DrawScene(int width, int height);

    D3D11RendererImpl* m_impl = nullptr;
};
