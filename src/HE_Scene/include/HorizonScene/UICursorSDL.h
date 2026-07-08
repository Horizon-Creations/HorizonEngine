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
    const int i = (int)c;
    if (i < 0 || i >= (int)UICursor::COUNT) return;

    SDL_SystemCursor sys;
    switch (c)
    {
        case UICursor::Hand:      sys = SDL_SYSTEM_CURSOR_POINTER;     break;
        case UICursor::Text:      sys = SDL_SYSTEM_CURSOR_TEXT;        break;
        case UICursor::Crosshair: sys = SDL_SYSTEM_CURSOR_CROSSHAIR;   break;
        case UICursor::ResizeWE:  sys = SDL_SYSTEM_CURSOR_EW_RESIZE;   break;
        case UICursor::ResizeNS:  sys = SDL_SYSTEM_CURSOR_NS_RESIZE;   break;
        case UICursor::Move:      sys = SDL_SYSTEM_CURSOR_MOVE;        break;
        case UICursor::No:        sys = SDL_SYSTEM_CURSOR_NOT_ALLOWED; break;
        case UICursor::Wait:      sys = SDL_SYSTEM_CURSOR_WAIT;        break;
        default:                  sys = SDL_SYSTEM_CURSOR_DEFAULT;     break; // Default / Arrow
    }
    if (!s_cache[i]) s_cache[i] = SDL_CreateSystemCursor(sys);
    if (s_cache[i]) SDL_SetCursor(s_cache[i]);
}

} // namespace HE
