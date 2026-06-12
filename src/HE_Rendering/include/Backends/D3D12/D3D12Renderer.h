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

private:
    D3D12RendererImpl* m_impl = nullptr;
};
