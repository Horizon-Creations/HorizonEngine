#pragma once
#include <cstdint>
#include "Types/Defines.h"
#include <SDL3/SDL.h>

// ── Input class ───────────────────────────────────────────────────────────────
// Keyboard-only, poll-based. SDL scancodes are used directly because SDL is
// already a public dependency of HorizonCore. Mouse input is not tracked here:
// the editor reads it from ImGui and the game reads it from InputMapping /
// SDL_GetMouseState, so there is no mouse state for this class to own.

class HE_API Input
{
public:
    // ── Polling ───────────────────────────────────────────────────────────
    // Per-frame check of the key state maintained by ProcessEvent.

    bool IsKeyDown(SDL_Scancode sc) const { return sc < SDL_SCANCODE_COUNT && m_keys[sc]; }

    // ── Internal — called by Application each frame ───────────────────────
    void ProcessEvent(const SDL_Event& event);

private:
    // Per-frame state
    bool m_keys[SDL_SCANCODE_COUNT]{};
};
