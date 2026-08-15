#pragma once
#include <cstdint>
#include "Types/Defines.h"
#include <SDL3/SDL.h>

// What the mouse did during one frame: movement since the last frame and wheel
// travel, both as DISPLACEMENTS. Which is the whole point — they are "how far",
// not "how fast", so game code must never multiply them by delta time the way it
// correctly does for a held key or a stick.
struct MouseFrame
{
    float dx = 0.0f, dy = 0.0f;
    float wheel = 0.0f;
};

// ── Input class ───────────────────────────────────────────────────────────────
// Keyboard state plus the frame's raw mouse movement. SDL scancodes are used
// directly because SDL is already a public dependency of HorizonCore.
//
// The mouse part is deliberately only the DEVICE stream — no buttons, no
// position, no notion of who is allowed to act on it. Absolute position and
// buttons are read where they are needed (ImGui in the editor, SDL_GetMouseState
// in the game); what could not be read anywhere was the movement, because
// SDL_GetRelativeMouseState DRAINS on read and the fly camera already consumes
// it. Accumulating from motion EVENTS is a second, independent stream, so this
// takes nothing away from that one.
class HE_API Input
{
public:
    // ── Polling ───────────────────────────────────────────────────────────
    // Per-frame check of the key state maintained by ProcessEvent.

    bool IsKeyDown(SDL_Scancode sc) const { return sc < SDL_SCANCODE_COUNT && m_keys[sc]; }

    // This frame's mouse movement. Readable as often as wanted — it is cleared
    // once, at the frame boundary, not by reading it.
    const MouseFrame& mouse() const { return m_mouse; }

    // ── Internal — called by Application each frame ───────────────────────
    void ProcessEvent(const SDL_Event& event);
    // Mouse motion and wheel, fed SEPARATELY and ungated. Key events reach
    // ProcessEvent only when the application did not consume them (ImGui gets
    // first refusal); the mouse stream is raw device data and the decision of
    // who may act on it belongs to the consumer — the game always, the editor
    // only while play mode holds the mouse.
    void ProcessMouseEvent(const SDL_Event& event);
    // Clear the frame's movement. Called by Application after the frame is
    // rendered, so everything drawing that frame sees the same numbers.
    void EndFrame() { m_mouse = MouseFrame{}; }

private:
    // Per-frame state
    bool       m_keys[SDL_SCANCODE_COUNT]{};
    MouseFrame m_mouse;
};
