#include "Backends/D3D11/D3D11Renderer.h"
#include <Window/Window.h>
#include <SDL3/SDL.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <stdexcept>
#include <Diagnostics/Logger.h>

using Microsoft::WRL::ComPtr;

struct D3D11RendererImpl
{
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGISwapChain>         swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    bool vsync = true;

    void createRTV()
    {
        ComPtr<ID3D11Texture2D> bb;
        swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                             reinterpret_cast<void**>(bb.GetAddressOf()));
        device->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    }
};

D3D11Renderer::D3D11Renderer()  : m_impl(new D3D11RendererImpl{}) {}
D3D11Renderer::~D3D11Renderer() { delete m_impl; }

void D3D11Renderer::Initialize(HE::Window* window)
{
    Logger::Log(Logger::LogLevel::Info, "D3D11Renderer: initializing");
    SDL_PropertiesID props = SDL_GetWindowProperties(window->GetNativeWindow());
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd)
        throw std::runtime_error("D3D11Renderer: could not get HWND");

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount                        = 1;
    scd.BufferDesc.Width                   = window->GetWidth();
    scd.BufferDesc.Height                  = window->GetHeight();
    scd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                       = hwnd;
    scd.SampleDesc.Count                   = 1;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &m_impl->swapchain,
        &m_impl->device, &fl, &m_impl->context);
    if (FAILED(hr))
        throw std::runtime_error("D3D11Renderer: D3D11CreateDeviceAndSwapChain failed");

    m_impl->createRTV();
    Logger::Log(Logger::LogLevel::Info, "D3D11Renderer: initialized successfully");
}

void D3D11Renderer::Shutdown()
{
    Logger::Log(Logger::LogLevel::Info, "D3D11Renderer: shutdown");
    m_impl->rtv.Reset();
    m_impl->swapchain.Reset();
    m_impl->context.Reset();
    m_impl->device.Reset();
}

void D3D11Renderer::Render()
{
    const float color[4] = { 0.18f, 0.18f, 0.20f, 1.0f };
    m_impl->context->ClearRenderTargetView(m_impl->rtv.Get(), color);
    if (m_overlayCallback) m_overlayCallback(nullptr);
    m_impl->swapchain->Present(m_impl->vsync ? 1 : 0, 0);
}

IRenderer::Capabilities D3D11Renderer::GetCapabilities() const { return { true, true, true }; }

void D3D11Renderer::SetVSync(bool enabled)
{
    Logger::Log(Logger::LogLevel::Info, enabled ? "D3D11Renderer: VSync enabled" : "D3D11Renderer: VSync disabled");
    m_impl->vsync = enabled;
}

void* D3D11Renderer::GetDevice()  const { return m_impl->device.Get(); }
void* D3D11Renderer::GetContext() const { return m_impl->context.Get(); }

void* D3D11Renderer::CreateImGuiTexture(const void* rgba8Pixels, int width, int height)
{
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width            = static_cast<UINT>(width);
	desc.Height           = static_cast<UINT>(height);
	desc.MipLevels        = 1;
	desc.ArraySize        = 1;
	desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage            = D3D11_USAGE_DEFAULT;
	desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA initData{};
	initData.pSysMem     = rgba8Pixels;
	initData.SysMemPitch = static_cast<UINT>(width * 4);

	ComPtr<ID3D11Texture2D> tex;
	if (FAILED(m_impl->device->CreateTexture2D(&desc, &initData, &tex)))
		return nullptr;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	ID3D11ShaderResourceView* srv = nullptr;
	if (FAILED(m_impl->device->CreateShaderResourceView(tex.Get(), &srvDesc, &srv)))
		return nullptr;

	return srv;
}

void D3D11Renderer::DestroyImGuiTexture(void* handle)
{
	if (!handle) return;
	static_cast<ID3D11ShaderResourceView*>(handle)->Release();
}
