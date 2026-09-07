#include <Renderer/RendererFactory.h>
// ─── What this runtime actually carries (docs/he-apps-plan.md A3b) ────────────
// The guards below are HE_BACKEND_*, set by src/HE_Rendering/CMakeLists.txt from
// the runtime flavour, and NOT the platform macros they replaced. The difference
// matters as soon as a runtime may deliberately carry less than its platform
// offers: "am I on Windows" used to answer "is D3D11 in this binary", and for an
// app runtime built with one renderer that answer is simply wrong. A game build
// gets exactly the old set, so nothing about it changes.
#ifdef HE_BACKEND_OPENGL
#include "../include/Backends/OpenGL/OpenGLRenderer.h"
#endif
#ifdef HE_BACKEND_SOFTWARE
#include "../include/Backends/Software/SoftwareRenderer.h"
#endif
#ifdef HE_BACKEND_VULKAN
#include "../include/Backends/Vulkan/VulkanRenderer.h"
#endif
#ifdef HE_BACKEND_D3D11
#include "../include/Backends/D3D11/D3D11Renderer.h"
#endif
#ifdef HE_BACKEND_D3D12
#include "../include/Backends/D3D12/D3D12Renderer.h"
#endif
#ifdef HE_BACKEND_METAL
#include "../include/Backends/Metal/MetalRenderer.h"
#endif
#include <stdexcept>

#ifndef HE_RUNTIME_FLAVOR_NAME
#define HE_RUNTIME_FLAVOR_NAME "game"
#endif

std::unique_ptr<IRenderer> RendererFactory::Create(HE::RendererBackend backend)
{
    switch (backend)
    {
#ifdef HE_BACKEND_OPENGL
        case HE::RendererBackend::OpenGL: return std::make_unique<OpenGLRenderer>();
#endif
#ifdef HE_BACKEND_SOFTWARE
        case HE::RendererBackend::Software: return std::make_unique<SoftwareRenderer>();
#endif
#ifdef HE_BACKEND_VULKAN
        case HE::RendererBackend::Vulkan: return std::make_unique<VulkanRenderer>();
#endif
#ifdef HE_BACKEND_D3D11
        case HE::RendererBackend::D3D11:  return std::make_unique<D3D11Renderer>();
#endif
#ifdef HE_BACKEND_D3D12
        case HE::RendererBackend::D3D12:  return std::make_unique<D3D12Renderer>();
#endif
#ifdef HE_BACKEND_METAL
        case HE::RendererBackend::Metal:  return std::make_unique<MetalRenderer>();
#endif
        default:
            throw std::runtime_error("RendererFactory: unknown backend");
    }
}

bool RendererFactory::Available(HE::RendererBackend backend)
{
    // Deliberately the same switch as Create, and deliberately in the same file:
    // every caller that asked this question with its own #ifdef ladder (the game
    // runtime had one) drifts the day a flavour changes the set.
    switch (backend)
    {
#ifdef HE_BACKEND_OPENGL
        case HE::RendererBackend::OpenGL:   return true;
#endif
#ifdef HE_BACKEND_SOFTWARE
        case HE::RendererBackend::Software: return true;
#endif
#ifdef HE_BACKEND_VULKAN
        case HE::RendererBackend::Vulkan:   return true;
#endif
#ifdef HE_BACKEND_D3D11
        case HE::RendererBackend::D3D11:    return true;
#endif
#ifdef HE_BACKEND_D3D12
        case HE::RendererBackend::D3D12:    return true;
#endif
#ifdef HE_BACKEND_METAL
        case HE::RendererBackend::Metal:    return true;
#endif
        default: return false;
    }
}

HE::RendererBackend RendererFactory::Default()
{
    // Preference order, first one that is IN this binary. Metal before OpenGL on
    // macOS is the old kDefaultBackend; Software last is what makes an app-basic
    // runtime start at all instead of throwing on a backend it never linked.
#ifdef HE_BACKEND_METAL
    return HE::RendererBackend::Metal;
#elif defined(HE_BACKEND_OPENGL)
    return HE::RendererBackend::OpenGL;
#elif defined(HE_BACKEND_D3D11)
    return HE::RendererBackend::D3D11;
#elif defined(HE_BACKEND_VULKAN)
    return HE::RendererBackend::Vulkan;
#else
    return HE::RendererBackend::Software;
#endif
}

const char* RendererFactory::RuntimeFlavor()
{
    // Logged at startup by the runtime. It is the only way to tell, from a build
    // that already ran, WHICH of the three flavours produced the numbers you are
    // looking at — and getting that wrong is exactly what ruined the first size
    // table in docs/he-apps-plan.md A3.
    return HE_RUNTIME_FLAVOR_NAME;
}
