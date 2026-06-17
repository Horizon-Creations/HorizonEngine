#pragma once
#include "Types/Defines.h"
#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

class Input;

// One key that triggers a named action.
struct ActionBinding
{
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
};

// A pair of keys that produce +scale / -scale on a named axis.
struct AxisBinding
{
    SDL_Scancode positiveKey = SDL_SCANCODE_UNKNOWN;
    SDL_Scancode negativeKey = SDL_SCANCODE_UNKNOWN;
    float        scale       = 1.0f;
};

// Per-frame state of a named action.
struct InputActionState
{
    bool isPressed    = false; // held this frame
    bool justPressed  = false; // went down this frame
    bool justReleased = false; // went up this frame
};

// Per-frame value of a named axis.
struct InputAxisState
{
    float value = 0.0f; // clamped to [-1, 1] after summing all bindings
};

// Maps logical action/axis names to one or more key bindings and maintains
// per-frame state. Call tick() once per frame after Input::ProcessEvent.
class HE_API InputMapping
{
public:
    void mapAction(std::string name, std::vector<ActionBinding> bindings);
    void mapAxis  (std::string name, std::vector<AxisBinding>   bindings);

    // Clear all mappings.
    void clear();

    // Update all action and axis states for this frame.
    void tick(const Input& input);

    // Returns nullptr if the action was never mapped.
    const InputActionState* getAction(const std::string& name) const;
    const InputAxisState*   getAxis  (const std::string& name) const;

    // Check without null-guard (returns default-constructed state for unknown names).
    bool isPressed   (const std::string& name) const;
    bool justPressed (const std::string& name) const;
    bool justReleased(const std::string& name) const;
    float axisValue  (const std::string& name) const;

    size_t actionCount() const { return m_actions.size(); }
    size_t axisCount()   const { return m_axes.size(); }

private:
    struct ActionEntry { std::vector<ActionBinding> bindings; InputActionState state; };
    struct AxisEntry   { std::vector<AxisBinding>   bindings; InputAxisState   state; };

    std::unordered_map<std::string, ActionEntry> m_actions;
    std::unordered_map<std::string, AxisEntry>   m_axes;
};
