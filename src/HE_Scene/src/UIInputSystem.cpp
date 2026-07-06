#include <HorizonScene/UIInputSystem.h>
#include <HorizonScene/UISystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/UICanvasComponent.h>
#include <HorizonScene/Components/UIElementComponent.h>
#include <HorizonScene/Components/UIButtonComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>

namespace UIInputSystem {

void update(HorizonWorld& world, InputState& state,
            float vpWidth, float vpHeight,
            float mouseX, float mouseY,
            bool primaryDown, bool pointerValid,
            std::vector<PointerEvent>& outEvents)
{
    auto& reg = world.registry();

    // Canvas scale: like UISystem::extract, the first active canvas wins
    // (typical scenes have exactly one).
    float scaleX = 1.0f, scaleY = 1.0f;
    bool haveCanvas = false;
    for (auto [e, canvas] : reg.view<UICanvasComponent>().each())
    {
        if (!canvas.active) continue;
        scaleX = vpWidth  / canvas.width;
        scaleY = vpHeight / canvas.height;
        haveCanvas = true;
        break;
    }

    // Topmost interactive element under the pointer — highest (layer, depth)
    // wins, matching the paint order.
    uint32_t top = 0;
    int topKey = 0;
    if (haveCanvas && pointerValid)
    {
        for (auto [e, elem] : reg.view<UIElementComponent>().each())
        {
            const bool interactive = reg.any_of<UIButtonComponent>(e) ||
                                     reg.any_of<ScriptComponent>(e);
            if (!interactive) continue;

            glm::vec2 pos, size;
            if (!UISystem::computeScreenRect(reg, e, vpWidth, vpHeight,
                                             scaleX, scaleY, pos, size))
                continue;
            if (mouseX < pos.x || mouseX > pos.x + size.x ||
                mouseY < pos.y || mouseY > pos.y + size.y)
                continue;

            int depth = 0;
            for (auto cur = e; depth < 255; )
            {
                const auto* h = reg.try_get<HierarchyComponent>(cur);
                if (!h || h->parent == entt::null ||
                    !reg.all_of<UIElementComponent>(h->parent)) break;
                cur = h->parent;
                ++depth;
            }
            const int key = elem.layer * 256 + depth;
            if (top == 0 || key >= topKey)
            {
                top = static_cast<uint32_t>(e);
                topKey = key;
            }
        }
    }

    // Hover transitions (at most one element is hovered — the topmost).
    std::unordered_set<uint32_t> nowHovered;
    if (top != 0) nowHovered.insert(top);
    for (uint32_t e : state.hovered)
        if (!nowHovered.count(e))
            outEvents.push_back({ e, PointerEvent::Type::HoverExit });
    for (uint32_t e : nowHovered)
        if (!state.hovered.count(e))
            outEvents.push_back({ e, PointerEvent::Type::HoverEnter });

    // Press / release / click.
    const bool pressEdge   = primaryDown && !state.wasDown;
    const bool releaseEdge = !primaryDown && state.wasDown;
    if (pressEdge)   state.pressed = top;
    if (releaseEdge)
    {
        if (state.pressed != 0 && state.pressed == top)
            outEvents.push_back({ top, PointerEvent::Type::Click });
        state.pressed = 0;
    }

    // Button visual states.
    for (auto [e, btn] : reg.view<UIButtonComponent>().each())
    {
        const uint32_t id = static_cast<uint32_t>(e);
        if (id == state.pressed && primaryDown)      btn.state = UIButtonState::Pressed;
        else if (nowHovered.count(id))               btn.state = UIButtonState::Hovered;
        else                                         btn.state = UIButtonState::Normal;
    }

    state.hovered = std::move(nowHovered);
    state.wasDown = primaryDown;
}

} // namespace UIInputSystem
