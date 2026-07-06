#pragma once
#include <entt/entt.hpp>
#include <cstdint>
#include <unordered_set>
#include <vector>

class HorizonWorld;

// Pointer input for in-game UI: hit-tests the mouse against interactive UI
// elements (buttons and script-carrying elements), drives UIButtonComponent
// hover/press states and emits pointer events for script dispatch
// (ScriptContext::callOnUIEvent). Pure ECS logic — the host app feeds it
// viewport-relative mouse coordinates in render-target pixels.
namespace UIInputSystem {

struct PointerEvent
{
    uint32_t entity = 0;
    enum class Type : uint8_t { Click, HoverEnter, HoverExit } type = Type::Click;
};

// Frame-to-frame pointer tracking, owned by the app (one per played world).
struct InputState
{
    std::unordered_set<uint32_t> hovered; // entities hovered last frame
    uint32_t pressed = 0;                 // entity the primary button went down on
    bool     wasDown = false;             // primary button state last frame
};

// Process one frame of pointer input. `pointerValid` is false while the mouse
// is captured (fly-look) or outside the viewport — hover states clear and no
// events fire. Click = press and release on the same topmost element.
void update(HorizonWorld& world, InputState& state,
            float vpWidth, float vpHeight,
            float mouseX, float mouseY,
            bool primaryDown, bool pointerValid,
            std::vector<PointerEvent>& outEvents);

} // namespace UIInputSystem
