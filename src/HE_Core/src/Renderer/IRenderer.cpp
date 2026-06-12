#include "Renderer/IRenderer.h"

void* IRenderer::CreateImGuiTexture(const void*, int, int) { return nullptr; }
void  IRenderer::DestroyImGuiTexture(void*)               {}
