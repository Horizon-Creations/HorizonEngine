#pragma once
#include <UIWidget/UIElement.h> // HE::UICursor
#include <SDL3/SDL.h>

// Header-only: map a UI element's requested HE::UICursor to an SDL system cursor
// and make it active. Shared by the game + editor apps (which both link SDL);
// HE::UICursor stays backend-agnostic in HE_Core. Cursors are created lazily and
// cached for the process lifetime.
namespace HE {

inline void applyUICursor(UICursor c)
{
    static SDL_Cursor* s_cache[(int)UICursor::COUNT] = {};
    // The application calls this EVERY frame, whether the pointer moved or not,
    // so the shape it asks for is the same one sixty times a second. SDL_SetCursor
    // has no early-out of its own and goes to the window system each time; the
    // remembered last shape is what keeps that from being a per-frame syscall.
    static int s_last = -1;
    const int i = (int)c;
    if (i < 0 || i >= (int)UICursor::COUNT || i == s_last) return;

    SDL_SystemCursor sys;
    switch (c)
    {
        case UICursor::Hand:      sys = SDL_SYSTEM_CURSOR_POINTER;     break;
        case UICursor::Text:      sys = SDL_SYSTEM_CURSOR_TEXT;        break;
        case UICursor::Crosshair: sys = SDL_SYSTEM_CURSOR_CROSSHAIR;   break;
        case UICursor::ResizeWE:  sys = SDL_SYSTEM_CURSOR_EW_RESIZE;   break;
        case UICursor::ResizeNS:  sys = SDL_SYSTEM_CURSOR_NS_RESIZE;   break;
        case UICursor::ResizeNWSE:sys = SDL_SYSTEM_CURSOR_NWSE_RESIZE; break;
        case UICursor::ResizeNESW:sys = SDL_SYSTEM_CURSOR_NESW_RESIZE; break;
        case UICursor::Move:      sys = SDL_SYSTEM_CURSOR_MOVE;        break;
        case UICursor::No:        sys = SDL_SYSTEM_CURSOR_NOT_ALLOWED; break;
        case UICursor::Wait:      sys = SDL_SYSTEM_CURSOR_WAIT;        break;
        default:                  sys = SDL_SYSTEM_CURSOR_DEFAULT;     break; // Default / Arrow
    }
    if (!s_cache[i]) s_cache[i] = SDL_CreateSystemCursor(sys);
    // Only a shape that was actually put on screen counts as the last one. A
    // cursor the platform would not create must stay retryable, or one failure
    // early on would freeze that shape out for the rest of the process.
    if (s_cache[i]) { SDL_SetCursor(s_cache[i]); s_last = i; }
}

} // namespace HE
