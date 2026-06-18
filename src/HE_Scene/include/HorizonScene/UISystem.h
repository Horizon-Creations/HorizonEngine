#pragma once
#include <Renderer/UIRenderObject.h>
#include <vector>
#include <cstdint>

class HorizonWorld;

namespace UISystem {

// Bake the embedded ProggyClean bitmap font into an 8-bit alpha atlas.
// outPixels is sized to width*height bytes (single-channel alpha).
// Returns false when stb_truetype cannot bake the atlas.
bool buildFontAtlas(int width, int height, float fontSizePixels,
                    std::vector<uint8_t>& outPixels);

// Walk all active canvases in world and fill out with UIRenderObjects.
// vpWidth/vpHeight are the current viewport dimensions in pixels.
void extract(HorizonWorld& world, float vpWidth, float vpHeight,
             std::vector<UIRenderObject>& out);

} // namespace UISystem
