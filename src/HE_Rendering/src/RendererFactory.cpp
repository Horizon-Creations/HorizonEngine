#include <Renderer/RendererFactory.h>
#include "../include/Backends/OpenGL/OpenGLRenderer.h"
#include "../include/Backends/Software/SoftwareRenderer.h"
#ifdef HE_VULKAN_ENABLED
#include "../include/Backends/Vulkan/VulkanRenderer.h"
#endif
#ifdef _WIN32
#include "../include/Backends/D3D11/D3D11Renderer.h"
#include "../include/Backends/D3D12/D3D12Renderer.h"
#endif
#ifdef __APPLE__
#include "../include/Backends/Metal/MetalRenderer.h"
#endif
#include <stdexcept>

std::unique_ptr<IRenderer> RendererFactory::Create(HE::RendererBackend backend)
{
    switch (backend)
    {
        case HE::RendererBackend::OpenGL: return std::make_unique<OpenGLRenderer>();
        // No #ifdef: a CPU rasterizer is available on every platform, which is
        // the whole point of it.
        case HE::RendererBackend::Software: return std::make_unique<SoftwareRenderer>();
#ifdef HE_VULKAN_ENABLED
        case HE::RendererBackend::Vulkan: return std::make_unique<VulkanRenderer>();
#endif
#ifdef _WIN32
        case HE::RendererBackend::D3D11:  return std::make_unique<D3D11Renderer>();
        case HE::RendererBackend::D3D12:  return std::make_unique<D3D12Renderer>();
#endif
#ifdef __APPLE__
        case HE::RendererBackend::Metal:  return std::make_unique<MetalRenderer>();
#endif
        default:
            throw std::runtime_error("RendererFactory: unknown backend");
    }
}
