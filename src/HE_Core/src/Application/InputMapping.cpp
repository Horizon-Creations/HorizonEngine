#include "Application/InputMapping.h"
#include <algorithm>
#include "Application/Input.h"

void InputMapping::mapAction(std::string name, std::vector<ActionBinding> bindings)
{
    m_actions[name].bindings = std::move(bindings);
}

void InputMapping::mapAxis(std::string name, std::vector<AxisBinding> bindings)
{
    AxisEntry& e = m_axes[name];
    e.bindings = std::move(bindings);
    e.yBindings.clear();
    e.is2D = false;
}

void InputMapping::mapAxis2D(std::string name, std::vector<AxisBinding> xBindings,
                             std::vector<AxisBinding> yBindings)
{
    AxisEntry& e = m_axes[name];
    e.bindings  = std::move(xBindings);
    e.yBindings = std::move(yBindings);
    e.is2D      = true;
}

void InputMapping::clear()
{
    m_actions.clear();
    m_axes.clear();
}

void InputMapping::tick(const Input& input, const MouseFrame& mouse)
{
    for (auto& [name, entry] : m_actions)
    {
        bool prev = entry.state.isPressed;
        bool cur  = false;
        for (auto& b : entry.bindings)
        {
            cur = cur || (b.key != SDL_SCANCODE_UNKNOWN && input.IsKeyDown(b.key));
            cur = cur || (b.gamepadButton != SDL_GAMEPAD_BUTTON_INVALID &&
                          input.isGamepadButtonDown(b.gamepadButton));
        }

        entry.state.isPressed    = cur;
        entry.state.justPressed  = cur  && !prev;
        entry.state.justReleased = !cur && prev;
    }

    // One axis component from its bindings.
    //
    // The keys are summed and clamped; the delta sources are added on top,
    // UNCLAMPED. Clamping the total would cap a fast mouse flick at one held
    // key's worth of turn, which is the whole reason mouse look could not be
    // expressed as an axis before. Clamping the keys on their own keeps the old
    // guarantee where it belonged: two key bindings on one axis cannot push a
    // held direction past full deflection.
    auto component = [&](const std::vector<AxisBinding>& binds)
    {
        float keys = 0.0f, delta = 0.0f;
        for (const AxisBinding& b : binds)
        {
            switch (b.source)
            {
            case AxisSource::Key:
                if (b.positiveKey != SDL_SCANCODE_UNKNOWN && input.IsKeyDown(b.positiveKey))
                    keys += b.scale;
                if (b.negativeKey != SDL_SCANCODE_UNKNOWN && input.IsKeyDown(b.negativeKey))
                    keys -= b.scale;
                if (b.positiveButton != SDL_GAMEPAD_BUTTON_INVALID &&
                    input.isGamepadButtonDown(b.positiveButton))
                    keys += b.scale;
                if (b.negativeButton != SDL_GAMEPAD_BUTTON_INVALID &&
                    input.isGamepadButtonDown(b.negativeButton))
                    keys -= b.scale;
                break;
            case AxisSource::MouseX:     delta += mouse.dx    * b.scale; break;
            case AxisSource::MouseY:     delta += mouse.dy    * b.scale; break;
            case AxisSource::MouseWheel: delta += mouse.wheel * b.scale; break;
            // Held states like the keys: they join the clamped sum, so a stick
            // plus a key bound to the same axis cannot exceed full deflection.
            case AxisSource::GamepadLeftX:
                keys += input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_LEFTX) * b.scale; break;
            case AxisSource::GamepadLeftY:
                keys += input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_LEFTY) * b.scale; break;
            case AxisSource::GamepadRightX:
                keys += input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_RIGHTX) * b.scale; break;
            case AxisSource::GamepadRightY:
                keys += input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_RIGHTY) * b.scale; break;
            case AxisSource::GamepadLeftTrigger:
                keys += input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * b.scale; break;
            case AxisSource::GamepadRightTrigger:
                keys += input.gamepadAxisFiltered(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * b.scale; break;
            }
        }
        return std::clamp(keys, -1.0f, 1.0f) + delta;
    };

    for (auto& [name, entry] : m_axes)
    {
        if (entry.is2D)
        {
            entry.state.x = component(entry.bindings);
            entry.state.y = component(entry.yBindings);
            entry.state.value = 0.0f;   // a 2D axis has no single value
        }
        else
        {
            entry.state.value = component(entry.bindings);
            entry.state.x = entry.state.y = 0.0f;
        }
    }
}

const InputActionState* InputMapping::getAction(const std::string& name) const
{
    auto it = m_actions.find(name);
    return it != m_actions.end() ? &it->second.state : nullptr;
}

const InputAxisState* InputMapping::getAxis(const std::string& name) const
{
    auto it = m_axes.find(name);
    return it != m_axes.end() ? &it->second.state : nullptr;
}

bool InputMapping::isPressed(const std::string& name) const
{
    auto* s = getAction(name);
    return s && s->isPressed;
}

bool InputMapping::justPressed(const std::string& name) const
{
    auto* s = getAction(name);
    return s && s->justPressed;
}

bool InputMapping::justReleased(const std::string& name) const
{
    auto* s = getAction(name);
    return s && s->justReleased;
}

float InputMapping::axisValue(const std::string& name) const
{
    auto* s = getAxis(name);
    return s ? s->value : 0.0f;
}

void InputMapping::axis2DValue(const std::string& name, float& x, float& y) const
{
    const InputAxisState* s = getAxis(name);
    x = s ? s->x : 0.0f;
    y = s ? s->y : 0.0f;
}
