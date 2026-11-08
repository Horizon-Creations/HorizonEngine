#include "Renderer/IRenderer.h"

#include <algorithm>
#include <atomic>

namespace
{
// A global and not a member: the lighting fill (HE_Rendering, FillMaterialWind) has no
// renderer to ask, and the quality setting is per process anyway. Defined here, in
// HE_Core, so every module reads the same one (a header-inline static would be one
// copy per DLL).
std::atomic<int> s_materialQuality{ 2 };
}

void IRenderer::SetQualitySettings(const QualitySettings& s)
{
    m_quality = s;
    s_materialQuality.store(std::clamp(s.materialQuality, 0, 2), std::memory_order_relaxed);
}

int IRenderer::MaterialQualityLevel() { return s_materialQuality.load(std::memory_order_relaxed); }

void* IRenderer::CreateImGuiTexture(const void*, int, int) { return nullptr; }
void  IRenderer::DestroyImGuiTexture(void*)               {}
