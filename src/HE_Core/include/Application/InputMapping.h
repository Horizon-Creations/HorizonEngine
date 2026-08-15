#pragma once
#include "Types/Defines.h"
#include "Application/Input.h"   // MouseFrame — tick() takes the frame's movement
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

// Where an axis binding takes its value from. Named so it extends: gamepad
// sticks and triggers belong in this list, they are just not built yet.
enum class AxisSource
{
    Key,        // the key PAIR below — the original and still the default
    MouseX,     // this frame's mouse movement, horizontal
    MouseY,     // …and vertical
    MouseWheel, // this frame's wheel movement
};

// True for the sources that report a DISPLACEMENT rather than a held state.
// The difference decides two things: such a source is not clamped (see tick),
// and game code must not multiply it by delta time — it is already "how far",
// not "how fast".
inline bool axisSourceIsDelta(AxisSource s) { return s != AxisSource::Key; }

// One contribution to a named axis: either a pair of keys producing
// +scale / -scale, or a mouse source scaled by the same factor.
struct AxisBinding
{
    SDL_Scancode positiveKey = SDL_SCANCODE_UNKNOWN;
    SDL_Scancode negativeKey = SDL_SCANCODE_UNKNOWN;
    float        scale       = 1.0f;
    // Last on purpose: `{ SDL_SCANCODE_D, SDL_SCANCODE_A }` is how a key pair
    // was written before there was anything else to bind, and appending keeps
    // every one of those spellings valid AND meaning what it meant.
    AxisSource   source      = AxisSource::Key;
};

// MouseFrame comes from Input (the device stream). It is PASSED IN to tick()
// rather than read off the Input reference this class already has, because who
// is allowed to act on the mouse differs by caller: a running game always may,
// the editor only while play mode holds it. Handing it in makes that an explicit
// decision at each call site instead of a rule to remember.

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
    // Key contributions summed and clamped to [-1, 1], PLUS every delta source
    // unclamped. Clamping the total would cap a fast mouse flick at one pixel's
    // worth of turn; clamping only the keys keeps a held key at full deflection
    // where a second binding cannot push it past 1.
    float value = 0.0f;
    // The same for a TWO-dimensional axis (Axis2D): x and y, each built by the
    // rule above. A 1D axis leaves this at zero and a 2D one leaves `value` at
    // zero — which one is meant is the action's declared type, not a guess.
    float x = 0.0f, y = 0.0f;
};

// Maps logical action/axis names to one or more key bindings and maintains
// per-frame state. Call tick() once per frame after Input::ProcessEvent.
class HE_API InputMapping
{
public:
    void mapAction(std::string name, std::vector<ActionBinding> bindings);
    void mapAxis  (std::string name, std::vector<AxisBinding>   bindings);
    // A TWO-dimensional axis: one binding list per component. Kept apart from
    // mapAxis rather than folded into it because an action is 1D or 2D by
    // declaration, and a reader asking axisValue() of a 2D axis (or axis2D() of
    // a 1D one) has made a mistake worth seeing as a zero rather than a guess.
    void mapAxis2D(std::string name, std::vector<AxisBinding> xBindings,
                   std::vector<AxisBinding> yBindings);

    // Clear all mappings.
    void clear();

    // Update all action and axis states for this frame. `mouse` is what the
    // mouse did since the last tick; pass {} where there is no mouse to give
    // (a headless test, or an editor frame that is not playing).
    void tick(const Input& input, const MouseFrame& mouse = {});

    // Returns nullptr if the action was never mapped.
    const InputActionState* getAction(const std::string& name) const;
    const InputAxisState*   getAxis  (const std::string& name) const;

    // Check without null-guard (returns default-constructed state for unknown names).
    bool isPressed   (const std::string& name) const;
    bool justPressed (const std::string& name) const;
    bool justReleased(const std::string& name) const;
    float axisValue  (const std::string& name) const;
    // The two components of a 2D axis (0,0 for an unknown or 1D one).
    void  axis2DValue(const std::string& name, float& x, float& y) const;

    size_t actionCount() const { return m_actions.size(); }
    size_t axisCount()   const { return m_axes.size(); }

private:
    struct ActionEntry { std::vector<ActionBinding> bindings; InputActionState state; };
    struct AxisEntry
    {
        std::vector<AxisBinding> bindings;    // the 1D axis, or a 2D axis's X
        std::vector<AxisBinding> yBindings;   // a 2D axis's Y (empty = 1D)
        bool                     is2D = false;
        InputAxisState           state;
    };

    std::unordered_map<std::string, ActionEntry> m_actions;
    std::unordered_map<std::string, AxisEntry>   m_axes;
};
