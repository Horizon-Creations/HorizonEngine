#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#include <cstdint>

#include <HorizonScene/UISystem.h>
#include <Renderer/UIFont.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/UICanvasComponent.h>
#include <HorizonScene/Components/UIElementComponent.h>
#include <HorizonScene/Components/UITextComponent.h>
#include <HorizonScene/Components/UIImageComponent.h>
#include <HorizonScene/Components/UIButtonComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <ProggyClean_ttf.h>

#include <cstring>
#include <algorithm>

namespace
{
    // Map an anchor enum to a normalized (0-1) canvas point.
    glm::vec2 anchorPoint(UIAnchor a)
    {
        switch (a)
        {
            case UIAnchor::TopLeft:      return {0.0f, 0.0f};
            case UIAnchor::TopCenter:    return {0.5f, 0.0f};
            case UIAnchor::TopRight:     return {1.0f, 0.0f};
            case UIAnchor::MiddleLeft:   return {0.0f, 0.5f};
            case UIAnchor::MiddleCenter: return {0.5f, 0.5f};
            case UIAnchor::MiddleRight:  return {1.0f, 0.5f};
            case UIAnchor::BottomLeft:   return {0.0f, 1.0f};
            case UIAnchor::BottomCenter: return {0.5f, 1.0f};
            case UIAnchor::BottomRight:  return {1.0f, 1.0f};
        }
        return {0.0f, 0.0f};
    }
}

namespace UISystem {

bool buildFontAtlas(int width, int height, float fontSizePixels,
                    std::vector<uint8_t>& outPixels)
{
    outPixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

    // Bake ASCII 32-127 into the atlas.
    std::vector<stbtt_bakedchar> charData(96);
    int result = stbtt_BakeFontBitmap(
        ProggyClean_data, 0,
        fontSizePixels,
        outPixels.data(), width, height,
        32, 96,
        charData.data()
    );

    // stbtt_BakeFontBitmap returns the number of first-row pixels used (>=0)
    // on success, or a negative number when glyphs overflow the atlas.
    return result > 0;
}

bool computeScreenRect(entt::registry& reg, entt::entity e,
                       float vpWidth, float vpHeight,
                       float scaleX, float scaleY,
                       glm::vec2& outPos, glm::vec2& outSize)
{
    const auto* elem = reg.try_get<UIElementComponent>(e);
    if (!elem || !elem->active) return false;

    // Parent rect: the nearest HierarchyComponent parent that is itself a UI
    // element; root elements anchor to the viewport. Recursion depth equals
    // UI nesting depth (reparentEntity guards against cycles).
    glm::vec2 parentPos{ 0.0f, 0.0f };
    glm::vec2 parentSize{ vpWidth, vpHeight };
    if (const auto* h = reg.try_get<HierarchyComponent>(e);
        h && h->parent != entt::null && reg.all_of<UIElementComponent>(h->parent))
    {
        if (!computeScreenRect(reg, h->parent, vpWidth, vpHeight,
                               scaleX, scaleY, parentPos, parentSize))
            return false; // inactive ancestor hides the whole subtree
    }

    const glm::vec2 ap = anchorPoint(elem->anchor);
    const glm::vec2 anchorScreen = parentPos + ap * parentSize;
    const glm::vec2 pivotOffset  = elem->pivot * elem->size * glm::vec2(scaleX, scaleY);
    outPos  = anchorScreen + elem->position * glm::vec2(scaleX, scaleY) - pivotOffset;
    outSize = elem->size * glm::vec2(scaleX, scaleY);
    return true;
}

void extract(HorizonWorld& world, float vpWidth, float vpHeight,
             std::vector<UIRenderObject>& out)
{
    auto& reg = world.registry();

    for (auto [canvasEnt, canvas] : reg.view<UICanvasComponent>().each())
    {
        if (!canvas.active) continue;

        const float scaleX = vpWidth  / canvas.width;
        const float scaleY = vpHeight / canvas.height;

        // Walk entities that are direct children of this canvas entity by
        // checking every UIElementComponent in the registry.  We match via
        // the HierarchyComponent parent field when present; fallback: include
        // all UI elements (editor scenes typically have one canvas).
        for (auto [elemEnt, elem] : reg.view<UIElementComponent>().each())
        {
            glm::vec2 screenPos, screenSize;
            if (!computeScreenRect(reg, elemEnt, vpWidth, vpHeight,
                                   scaleX, scaleY, screenPos, screenSize))
                continue;

            // Children draw over their parents at equal layer: the sort key is
            // layer (major) + UI nesting depth (minor). The view iterates in
            // arbitrary order, so without the depth key a child could vanish
            // under its parent panel.
            int depth = 0;
            for (auto cur = elemEnt; depth < 255; )
            {
                const auto* h = reg.try_get<HierarchyComponent>(cur);
                if (!h || h->parent == entt::null ||
                    !reg.all_of<UIElementComponent>(h->parent)) break;
                cur = h->parent;
                ++depth;
            }
            const int sortLayer = elem.layer * 256 + depth;

            auto* img = reg.try_get<UIImageComponent>(elemEnt);
            auto* btn = reg.try_get<UIButtonComponent>(elemEnt);

            // Image quad. When the entity is also a button, the button state
            // drives the quad color (a separate solid button quad would draw
            // over the image and hide its material).
            if (img)
            {
                glm::vec4 col = img->tint;
                if (btn)
                {
                    col = btn->normalColor;
                    if (btn->state == UIButtonState::Hovered)  col = btn->hoveredColor;
                    if (btn->state == UIButtonState::Pressed)  col = btn->pressedColor;
                    col *= img->tint;
                }
                UIRenderObject ro;
                ro.position        = screenPos;
                ro.size            = screenSize;
                ro.color           = col;
                ro.materialAssetId = img->materialAssetId;
                ro.type            = 0;
                ro.layer           = sortLayer;
                out.push_back(std::move(ro));
            }

            // Plain button quad (no image on the entity).
            if (btn && !img)
            {
                glm::vec4 col = btn->normalColor;
                if (btn->state == UIButtonState::Hovered)  col = btn->hoveredColor;
                if (btn->state == UIButtonState::Pressed)  col = btn->pressedColor;

                UIRenderObject ro;
                ro.position = screenPos;
                ro.size     = screenSize;
                ro.color    = col;
                ro.type     = 0;
                ro.layer    = sortLayer;
                out.push_back(std::move(ro));
            }

            // Text run: emitted as per-glyph atlas quads (type 2). Button labels
            // (a UIText on a button entity) center in the quad; free-standing
            // text is left-aligned. Font size is authored in canvas units →
            // scaled to pixels like every other vertical extent.
            if (auto* txt = reg.try_get<UITextComponent>(elemEnt))
            {
                HE::emitUITextGlyphs(txt->text, screenPos, screenSize,
                                     txt->fontSize * scaleY, txt->color,
                                     sortLayer + (btn ? 1 : 0), btn != nullptr, out);
            }
        }
    }

    // Sort by layer so deeper elements draw first (painter's algorithm).
    std::stable_sort(out.begin(), out.end(), [](const UIRenderObject& a, const UIRenderObject& b){
        return a.layer < b.layer;
    });
}

} // namespace UISystem
