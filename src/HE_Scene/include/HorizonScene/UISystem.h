#pragma once
#include <Renderer/UIRenderObject.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

class HorizonWorld;

namespace UISystem {

// Bake the embedded ProggyClean bitmap font into an 8-bit alpha atlas.
// outPixels is sized to width*height bytes (single-channel alpha).
// Returns false when stb_truetype cannot bake the atlas.
bool buildFontAtlas(int width, int height, float fontSizePixels,
                    std::vector<uint8_t>& outPixels);

// The shared baked UI font + glyph emission live in HE_Core (Renderer/UIFont.h)
// so renderer backends can upload the atlas without a scene dependency;
// extract() lays out text through HE::emitUITextGlyphs.

// Resolve an element's screen-space rect. Anchoring is parent-relative: when
// the entity's HierarchyComponent parent is itself a UI element, the anchor
// resolves inside the parent's rect; root elements anchor to the viewport.
// scaleX/scaleY map canvas units → pixels. Returns false when the entity has
// no active UIElementComponent or any UI ancestor is inactive.
bool computeScreenRect(entt::registry& reg, entt::entity e,
                       float vpWidth, float vpHeight,
                       float scaleX, float scaleY,
                       glm::vec2& outPos, glm::vec2& outSize);

// Walk all active canvases in world and fill out with UIRenderObjects.
// vpWidth/vpHeight are the current viewport dimensions in pixels.
void extract(HorizonWorld& world, float vpWidth, float vpHeight,
             std::vector<UIRenderObject>& out);

} // namespace UISystem
