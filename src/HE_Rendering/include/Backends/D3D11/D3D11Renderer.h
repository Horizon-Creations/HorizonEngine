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

private:
    // Extract → cull → sort → RenderGraph → replay into the bound targets.
    void DrawScene(int width, int height);

    D3D11RendererImpl* m_impl = nullptr;
};
