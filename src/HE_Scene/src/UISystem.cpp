#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <HorizonScene/UISystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/UICanvasComponent.h>
#include <HorizonScene/Components/UIElementComponent.h>
#include <HorizonScene/Components/UITextComponent.h>
#include <HorizonScene/Components/UIImageComponent.h>
#include <HorizonScene/Components/UIButtonComponent.h>
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
            if (!elem.active) continue;

            const glm::vec2 ap = anchorPoint(elem.anchor);
            // Anchor position in screen pixels.
            const glm::vec2 anchorScreen = ap * glm::vec2(vpWidth, vpHeight);
            // Pivot offset: shift so pivot is at anchorScreen + position.
            const glm::vec2 pivotOffset  = elem.pivot * elem.size * glm::vec2(scaleX, scaleY);
            const glm::vec2 screenPos    =
                anchorScreen
                + elem.position * glm::vec2(scaleX, scaleY)
                - pivotOffset;
            const glm::vec2 screenSize   = elem.size * glm::vec2(scaleX, scaleY);

            // Image quad.
            if (auto* img = reg.try_get<UIImageComponent>(elemEnt))
            {
                UIRenderObject ro;
                ro.position        = screenPos;
                ro.size            = screenSize;
                ro.color           = img->tint;
                ro.materialAssetId = img->materialAssetId;
                ro.type            = 0;
                ro.layer           = elem.layer;
                out.push_back(std::move(ro));
            }

            // Button quad (uses button's current color).
            if (auto* btn = reg.try_get<UIButtonComponent>(elemEnt))
            {
                glm::vec4 col = btn->normalColor;
                if (btn->state == UIButtonState::Hovered)  col = btn->hoveredColor;
                if (btn->state == UIButtonState::Pressed)  col = btn->pressedColor;

                UIRenderObject ro;
                ro.position = screenPos;
                ro.size     = screenSize;
                ro.color    = col;
                ro.type     = 0;
                ro.layer    = elem.layer;
                out.push_back(std::move(ro));
            }

            // Text quad.
            if (auto* txt = reg.try_get<UITextComponent>(elemEnt))
            {
                UIRenderObject ro;
                ro.position  = screenPos;
                ro.size      = screenSize;
                ro.color     = txt->color;
                ro.text      = txt->text;
                ro.fontSize  = txt->fontSize;
                ro.type      = 1;
                ro.layer     = elem.layer;
                out.push_back(std::move(ro));
            }
        }
    }

    // Sort by layer so deeper elements draw first (painter's algorithm).
    std::stable_sort(out.begin(), out.end(), [](const UIRenderObject& a, const UIRenderObject& b){
        return a.layer < b.layer;
    });
}

} // namespace UISystem
