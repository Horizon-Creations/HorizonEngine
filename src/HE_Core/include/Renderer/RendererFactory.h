#pragma once
#include "Types/Defines.h"
#include "Types/Enums.h"
#include "Renderer/IRenderer.h"
#include <memory>

// ─── RendererFactory ──────────────────────────────────────────────────────────
// Declaration only. Implementation lives in HorizonRendering so backends
// (Vulkan, D3D12, …) never leak into HorizonCore.

class HE_API RendererFactory
{
public:
    // Implemented in HorizonRendering/src/RendererFactory.cpp
    static std::unique_ptr<IRenderer> Create(HE::RendererBackend backend);
};
