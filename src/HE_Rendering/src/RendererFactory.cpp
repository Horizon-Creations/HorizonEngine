#include <Renderer/RendererFactory.h>
#include "../include/Backends/OpenGL/OpenGLRenderer.h"
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

std::unique_ptr<IRenderer> RendererFactory::Create(Backend backend)
{
    switch (backend)
    {
        case Backend::OpenGL: return std::make_unique<OpenGLRenderer>();
#ifdef HE_VULKAN_ENABLED
        case Backend::Vulkan: return std::make_unique<VulkanRenderer>();
#endif
#ifdef _WIN32
        case Backend::D3D11:  return std::make_unique<D3D11Renderer>();
        case Backend::D3D12:  return std::make_unique<D3D12Renderer>();
#endif
#ifdef __APPLE__
        case Backend::Metal:  return std::make_unique<MetalRenderer>();
#endif
        default:
            throw std::runtime_error("RendererFactory: unknown backend");
    }
}
