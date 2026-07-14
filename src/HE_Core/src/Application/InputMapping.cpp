#include "Application/InputMapping.h"
#include <algorithm>
#include "Application/Input.h"

void InputMapping::mapAction(std::string name, std::vector<ActionBinding> bindings)
{
    m_actions[name].bindings = std::move(bindings);
}

void InputMapping::mapAxis(std::string name, std::vector<AxisBinding> bindings)
{
    m_axes[name].bindings = std::move(bindings);
}

void InputMapping::clear()
{
    m_actions.clear();
    m_axes.clear();
}

void InputMapping::tick(const Input& input)
{
    for (auto& [name, entry] : m_actions)
    {
        bool prev = entry.state.isPressed;
        bool cur  = false;
        for (auto& b : entry.bindings)
            cur = cur || (b.key != SDL_SCANCODE_UNKNOWN && input.IsKeyDown(b.key));

        entry.state.isPressed    = cur;
        entry.state.justPressed  = cur  && !prev;
        entry.state.justReleased = !cur && prev;
    }

    for (auto& [name, entry] : m_axes)
    {
        float v = 0.0f;
        for (auto& b : entry.bindings)
        {
            if (b.positiveKey != SDL_SCANCODE_UNKNOWN && input.IsKeyDown(b.positiveKey))
                v += b.scale;
            if (b.negativeKey != SDL_SCANCODE_UNKNOWN && input.IsKeyDown(b.negativeKey))
                v -= b.scale;
        }
        entry.state.value = std::clamp(v, -1.0f, 1.0f);
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
