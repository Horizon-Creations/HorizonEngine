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
    // Throws for a backend this runtime was not built with — ask Available first.
    static std::unique_ptr<IRenderer> Create(HE::RendererBackend backend);

    // Is that backend compiled into THIS runtime? A shipped app runtime carries
    // one renderer on purpose (docs/he-apps-plan.md A3b), so the answer is no
    // longer "whatever the platform offers" and callers must not derive it from
    // __APPLE__ / _WIN32 themselves — that is how a config.json written for
    // another build would kill the process before its window opens.
    static bool Available(HE::RendererBackend backend);

    // The backend to use when nothing valid was asked for: the platform's
    // preferred one among those actually linked.
    static HE::RendererBackend Default();

    // "game", "app-advanced" or "app-basic" — which of the three runtime
    // flavours this binary is. Logged at startup so a measurement or a bug
    // report says which one it belongs to.
    static const char* RuntimeFlavor();
};
